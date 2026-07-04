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
    void resolve(const ast::Program& program, Handle runScope) {
        scopes_.emplace_back();
        const auto& env = static_cast<const EnvValue&>(vm_.arena().deref(runScope));
        for (const auto& [name, h] : env.locals()) declare(name);  // arglist/argmain, prior REPL, embedder
        collectDecls(program.stmts);
        checkBlock(program.stmts);
        scopes_.pop_back();
    }

private:
    KiritoVM& vm_;
    std::vector<fum::unordered_set<std::string>> scopes_;
    int depth_ = 0;  // checkExpr recursion bound (anti stack-overflow)

    void declare(const std::string& name) { scopes_.back().insert(name); }
    bool inLexicalScope(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            if (it->count(name)) return true;
        return false;
    }
    bool isGlobal(const std::string& name) const {
        return envLookup(vm_.arena(), vm_.global(), name).has_value();  // builtins + registered globals
    }

    // Open a nested scope (a function or class body), predeclare its params, gather ALL of the
    // block's declarations (so forward references resolve), check every reference, then close it.
    void resolveScope(const ast::Block& body, const std::vector<ast::Param>* params = nullptr) {
        scopes_.emplace_back();
        if (params) for (const auto& p : *params) declare(p.name);
        collectDecls(body);
        checkBlock(body);
        scopes_.pop_back();
    }

    // Pre-pass: record every name a block binds into the current scope. The traversal lives in
    // locals.hpp (collectBlockDecls), shared with the compiler's slot-assignment pass so both agree on
    // exactly which names a scope binds.
    void collectDecls(const ast::Block& block) { kirito::collectBlockDecls(block, scopes_.back()); }

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
            resolveScope(c->body);  // a class body is its own (membership) scope
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
        if (!inLexicalScope(n.name) && !isGlobal(n.name))
            throw KiritoError("name '" + n.name + "' is not defined", n.span);
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
            for (const auto& p : fn->params) if (p.defaultValue) checkExpr(*p.defaultValue);  // defaults: enclosing scope
            resolveScope(fn->body, &fn->params);
        }
        // LiteralExpr binds/references nothing.
    }
};

}  // namespace kirito

#endif
