#ifndef KIRITO_RESOLVER_HPP
#define KIRITO_RESOLVER_HPP

// Compile-time, scope-aware name resolution. Runs once over a parsed Program (before compilation),
// and THROWS a KiritoError for any name reference that does not resolve to a binding — a parameter,
// a `var`/`for`/`class`/`catch`/`with` name in the current or an enclosing lexical scope, the
// per-file `arglist`/`argmain`, or a global/builtin installed on the VM. Resolution is by scope
// MEMBERSHIP, not textual order: a name declared anywhere in a scope is visible throughout it, so
// recursion, mutual recursion, and forward references all resolve. Only a name bound nowhere
// reachable is an error — caught here, at compile time, instead of as a runtime NameError.
//
// Only functions, the module/program, and class bodies introduce scopes (if/while/for/try/with/switch
// blocks share their enclosing scope, matching the runtime). A bare `x = ...` is a rebind, not a
// declaration, so it must resolve like any other reference.

#include <string>
#include <vector>

#include "fum/unordered_map.hpp"
#include "fum/unordered_set.hpp"
#include "ast.hpp"
#include "common.hpp"
#include "environment.hpp"
#include "locals.hpp"
#include "vm.hpp"

namespace kirito {

class Resolver {
public:
    explicit Resolver(KiritoVM& vm) : vm_(vm) {}

    // Resolve the program that will run in `runScope`. Names already bound there are predeclared, so
    // the REPL's accumulated bindings (a persistent scope across lines) and any embedder-defined
    // names resolve — not just the ones this program declares. Builtins resolve via the global env.
    //
    // `indexTopLevel` is set for a genuine module/script scope (a directly-run file, an imported .ki
    // file, a frozen stdlib module) — NOT the REPL's persistent dynamic scope nor a bare embedder
    // eval. When set, every top-level binding is given a fixed slot in the live module EnvValue up
    // front (predeclareModuleSlots) and every reference that resolves to one is annotated with its
    // (depth, index) so the compiler emits a direct LoadVar/AssignVar with no run-time name walk.
    void resolve(const ast::Program& program, Handle runScope, bool indexTopLevel = false) {
        indexTopLevel_ = indexTopLevel;
        scopes_.emplace_back();
        const auto& env = static_cast<const EnvValue&>(vm_.arena().deref(runScope));
        for (const auto& [name, h] : env.locals()) declare(name);  // arglist/argmain, prior REPL, embedder
        collectDecls(program.stmts);
        if (indexTopLevel_) predeclareModuleSlots(runScope);
        checkBlock(program.stmts);
        scopes_.pop_back();
    }

private:
    // A lexical scope on the resolution stack. `names` is membership (a name declared anywhere in the
    // scope is visible throughout it). `kind` distinguishes the module/REPL scope, a function body, and
    // a class body. `envIndex` maps each name that lives in this scope's EnvValue to its fixed slot —
    // for a module scope, every top-level binding (when indexed); for a function scope, its captured
    // parameters (at their positional index) and captured locals (after the parameters). A reference
    // resolving to a name in `envIndex` compiles to a direct LoadVar/AssignVar(depth, index); a name
    // absent from it is a frame-slot local (LoadLocal) or, in a bare-eval/class scope, name-based.
    struct Scope {
        fum::unordered_set<std::string> names;
        enum Kind { Module, Function, Class } kind = Module;
        fum::unordered_map<std::string, uint32_t> envIndex;
    };
    KiritoVM& vm_;
    std::vector<Scope> scopes_;
    int depth_ = 0;  // checkExpr recursion bound (anti stack-overflow)
    bool indexTopLevel_ = false;                        // module-scope names get fixed slots + LoadVar
    int inDefault_ = 0;                                 // >0 while checking a parameter default (its own frame)

    void declare(const std::string& name) { scopes_.back().names.insert(name); }
    // Index of the innermost scope (0 == module) that declares `name`, or -1 if none — so the caller
    // can compute the EnvValue-hop depth to it. Every enclosing function/class body is exactly one hop.
    int lexicalScopeIndexOf(const std::string& name) const {
        for (std::size_t j = scopes_.size(); j-- > 0;)
            if (scopes_[j].names.count(name)) return static_cast<int>(j);
        return -1;
    }
    bool isGlobal(const std::string& name) const {
        return envLookup(vm_.arena(), vm_.global(), name).has_value();  // builtins + registered globals
    }

    // Give every top-level binding a fixed slot in the live module EnvValue before the body runs.
    // Seeded names (arglist/argmain, plus anything the embedder/REPL carried in) keep their existing
    // positions; each remaining declaration is appended as an `undefined()` placeholder that its `var`
    // overwrites in place at run time. Indices are read straight back from the scope, so a reference
    // annotated from scope0Index_ is guaranteed to address the right slot — no name lookup needed. A
    // name never assigned at run time stays `undefined()` and is filtered out of the module's exports.
    void predeclareModuleSlots(Handle runScope) {
        auto& env = static_cast<EnvValue&>(vm_.arena().deref(runScope));
        auto& idx = scopes_[0].envIndex;
        for (std::size_t i = 0; i < env.size(); ++i) idx.emplace(env.nameAt(i), static_cast<uint32_t>(i));
        for (const auto& name : scopes_[0].names) {
            if (idx.count(name)) continue;  // already bound (seeded, or a duplicate declaration)
            uint32_t slot = static_cast<uint32_t>(env.size());
            env.define(name, vm_.undefined());
            idx.emplace(name, slot);
        }
    }

    // A function body: its own scope. Predeclare its params, gather ALL of the body's declarations (so
    // forward references resolve), assign env slots to its captured params/locals, check every
    // reference, then close it.
    void resolveFunctionScope(const ast::FunctionExpr& fn) {
        scopes_.emplace_back();
        scopes_.back().kind = Scope::Function;
        for (const auto& p : fn.params) declare(p.name);
        collectDecls(fn.body);
        computeFunctionEnvIndex(scopes_.back(), fn);
        checkBlock(fn.body);
        scopes_.pop_back();
    }
    // A class body is its own membership scope, but its bindings stay name-based (they are harvested by
    // name into the class's method/attr table); indexing them is Step 4, so it gets no envIndex.
    void resolveClassScope(const ast::Block& body) {
        scopes_.emplace_back();
        scopes_.back().kind = Scope::Class;
        collectDecls(body);
        checkBlock(body);
        scopes_.pop_back();
    }

    // The env layout of a function scope: captured parameters keep their positional slot (callFull
    // defines every parameter in the scope env, in order), and captured non-param locals follow the
    // parameters in the SAME deterministic order the compiler records in Proto.envSlots and the runtime
    // pre-declares them (collectBlockDeclsOrdered + captured/non-param filter) — so index P+i names the
    // same binding on both sides. Non-captured locals/params are frame slots and get no env index.
    void computeFunctionEnvIndex(Scope& s, const ast::FunctionExpr& fn) {
        NameSet captured = capturedLocals(fn.params, fn.body);
        NameSet paramSet;
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            paramSet.insert(fn.params[i].name);
            if (captured.count(fn.params[i].name))
                s.envIndex.emplace(fn.params[i].name, static_cast<uint32_t>(i));
        }
        uint32_t next = static_cast<uint32_t>(fn.params.size());
        for (const auto& name : collectBlockDeclsOrdered(fn.body))
            if (captured.count(name) && !paramSet.count(name)) s.envIndex.emplace(name, next++);
    }

    // Pre-pass: record every name a block binds into the current scope. The traversal lives in
    // locals.hpp (collectBlockDecls), shared with the compiler's slot-assignment pass so both agree on
    // exactly which names a scope binds.
    void collectDecls(const ast::Block& block) { kirito::collectBlockDecls(block, scopes_.back().names); }

    // --- reference checking ---------------------------------------------------------------------
    void checkBlock(const ast::Block& block) { for (const auto& s : block) checkStmt(*s); }

    void checkStmt(const ast::Stmt& s) {
        if (const auto* e = dynamic_cast<const ast::ExprStmt*>(&s)) checkExpr(*e->expr);
        else if (const auto* d = dynamic_cast<const ast::DiscardStmt*>(&s)) checkExpr(*d->expr);
        else if (const auto* v = dynamic_cast<const ast::VarDeclStmt*>(&s)) checkExpr(*v->init);
        else if (const auto* a = dynamic_cast<const ast::AssignStmt*>(&s)) { checkExpr(*a->value); checkTarget(*a->target); }
        else if (const auto* i = dynamic_cast<const ast::IfStmt*>(&s)) {
            for (const auto& [cond, b] : i->branches) { checkExpr(*cond); checkBlock(b); }
            if (i->orelse) checkBlock(*i->orelse);
        } else if (const auto* w = dynamic_cast<const ast::WhileStmt*>(&s)) { checkExpr(*w->cond); checkBlock(w->body); }
        else if (const auto* f = dynamic_cast<const ast::ForStmt*>(&s)) { checkExpr(*f->iterable); checkBlock(f->body); }
        else if (const auto* r = dynamic_cast<const ast::ReturnStmt*>(&s)) { if (r->value) checkExpr(*r->value); }
        else if (const auto* t = dynamic_cast<const ast::TryStmt*>(&s)) {
            checkBlock(t->body);
            for (const auto& h : t->handlers) { if (h.type) checkExpr(*h.type); checkBlock(h.body); }
            if (t->hasFinally) checkBlock(t->finallyBody);
        } else if (const auto* th = dynamic_cast<const ast::ThrowStmt*>(&s)) checkExpr(*th->value);
        else if (const auto* c = dynamic_cast<const ast::ClassStmt*>(&s)) {
            if (c->base) checkExpr(*c->base);
            resolveClassScope(c->body);  // a class body is its own (membership) scope
        } else if (const auto* wi = dynamic_cast<const ast::WithStmt*>(&s)) { checkExpr(*wi->context); checkBlock(wi->body); }
        else if (const auto* as = dynamic_cast<const ast::AssertStmt*>(&s)) { checkExpr(*as->cond); if (as->message) checkExpr(*as->message); }
        else if (const auto* sw = dynamic_cast<const ast::SwitchStmt*>(&s)) {
            checkExpr(*sw->subject);
            for (const auto& cl : sw->cases) { for (const auto& cv : cl.values) checkExpr(*cv); checkBlock(cl.body); }
            if (sw->hasDefault) checkBlock(sw->defaultBody);
        }
        // Break / Continue / Pass / Todo reference nothing.
    }

    // An assignment target: a bare name must resolve (it rebinds an existing binding); index/member
    // targets read their object/keys; tuple/star recurse.
    void checkTarget(const ast::Expr& target) {
        ast::walkAssignTarget(target,
            [&](const ast::NameExpr& n) { checkName(n); },     // a bare-name target must resolve (rebind)
            [&](const ast::Expr& e) { checkExpr(e); });        // index/member objects + keys are read
    }

    void checkName(const ast::NameExpr& n) {
        int j = lexicalScopeIndexOf(n.name);
        if (j >= 0) {                          // a lexical binding shadows any builtin
            // Resolve to a direct env slot when the owning scope indexes this name: a function scope
            // always indexes its captured params/locals; the module scope indexes its top-level names
            // only for a real module/script/REPL scope (indexTopLevel_), never a bare embedder eval. A
            // frame-slot local (absent from envIndex) stays LoadLocal; a class-body name stays
            // name-based (Step 4). depth = the EnvValue hops from the referencing frame up to the owning
            // scope (each enclosing function/class body is one hop). Parameter defaults run as their own
            // Proto (a different frame/depth), so references inside them are left name-based.
            const Scope& sj = scopes_[static_cast<std::size_t>(j)];
            bool indexable = sj.kind == Scope::Function || (sj.kind == Scope::Module && indexTopLevel_);
            if (indexable && inDefault_ == 0) {
                auto it = sj.envIndex.find(n.name);
                if (it != sj.envIndex.end()) {
                    n.envDepth = static_cast<int>(scopes_.size() - 1 - static_cast<std::size_t>(j));
                    n.envIndex = static_cast<int>(it->second);
                }
            }
            return;
        }
        if (!isGlobal(n.name))
            throw KiritoError("name '" + n.name + "' is not defined", n.span);
        // A genuine builtin/global reference: annotate its fixed global slot so the compiler emits a
        // direct LoadGlobal. builtinSlot() is -1 for an embedder-added global (still resolves via LoadName).
        n.builtinSlot = vm_.builtinSlot(n.name);
    }

    void checkExpr(const ast::Expr& e) {
        if (depth_ > 2800) return;  // best-effort bound; the compiler's own guard throws on real over-nesting
        ++depth_;
        struct Pop { int& d; ~Pop() { --d; } } pop{depth_};
        if (const auto* n = dynamic_cast<const ast::NameExpr*>(&e)) checkName(*n);
        else if (const auto* u = dynamic_cast<const ast::UnaryExpr*>(&e)) checkExpr(*u->operand);
        else if (const auto* b = dynamic_cast<const ast::BinaryExpr*>(&e)) { checkExpr(*b->lhs); checkExpr(*b->rhs); }
        else if (const auto* l = dynamic_cast<const ast::LogicalExpr*>(&e)) { checkExpr(*l->lhs); checkExpr(*l->rhs); }
        else if (const auto* cn = dynamic_cast<const ast::ConditionalExpr*>(&e)) { checkExpr(*cn->cond); checkExpr(*cn->then); checkExpr(*cn->orelse); }
        else if (const auto* c = dynamic_cast<const ast::CallExpr*>(&e)) { checkExpr(*c->callee); for (const auto& a : c->args) checkExpr(*a.value); }
        else if (const auto* m = dynamic_cast<const ast::MemberExpr*>(&e)) {
            if (m->name == "_super_") { checkExpr(*m->object); return; }  // the super proxy, not a member name
            checkExpr(*m->object);  // .name is a member, resolved at run time on the object, not a scope name
        } else if (const auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            checkExpr(*ix->object);
            for (const auto& k : ix->indices) checkExpr(*k);
        } else if (const auto* sl = dynamic_cast<const ast::SliceExpr*>(&e)) {
            checkExpr(*sl->object);
            if (sl->start) checkExpr(*sl->start);
            if (sl->stop) checkExpr(*sl->stop);
            if (sl->step) checkExpr(*sl->step);
        } else if (const auto* lst = dynamic_cast<const ast::ListLiteral*>(&e)) { for (const auto& x : lst->elems) checkExpr(*x); }
        else if (const auto* st = dynamic_cast<const ast::SetLiteral*>(&e)) { for (const auto& x : st->elems) checkExpr(*x); }
        else if (const auto* dt = dynamic_cast<const ast::DictLiteral*>(&e)) { for (const auto& [k, v] : dt->entries) { checkExpr(*k); checkExpr(*v); } }
        else if (const auto* fs = dynamic_cast<const ast::FStringExpr*>(&e)) { for (const auto& p : fs->parts) if (p.isExpr) checkExpr(*p.expr); }
        else if (const auto* tup = dynamic_cast<const ast::TupleExpr*>(&e)) { for (const auto& x : tup->elems) checkExpr(*x); }
        else if (const auto* star = dynamic_cast<const ast::StarExpr*>(&e)) checkExpr(*star->inner);
        else if (const auto* fn = dynamic_cast<const ast::FunctionExpr*>(&e)) {
            // A param default is evaluated in the enclosing (call) scope, but the binder defines the
            // params LEFT-TO-RIGHT, so a default may reference an EARLIER param — `Function(n, size = n)`
            // (A03-3). Check each default with the preceding params visible (a temporary innermost
            // scope populated incrementally), then resolve the body with all params.
            scopes_.emplace_back();
            for (const auto& p : fn->params) {
                // A default runs as its own Proto in the call scope (a different frame/depth than the
                // body), so references inside it are left name-based, not annotated for LoadVar.
                if (p.defaultValue) { ++inDefault_; checkExpr(*p.defaultValue); --inDefault_; }
                declare(p.name);                                   // visible to the defaults that follow
            }
            scopes_.pop_back();
            resolveFunctionScope(*fn);
        }
        // LiteralExpr binds/references nothing.
    }
};

}  // namespace kirito

#endif
