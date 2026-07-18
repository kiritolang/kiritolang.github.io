#ifndef KIRITO_ANALYZER_HPP
#define KIRITO_ANALYZER_HPP

// Static analysis (lint) pass over the AST. Runs once after parsing, before execution, and reports
// non-fatal warnings to be printed to stderr by the caller. It is deliberately SOUND-leaning: it
// only warns when confident, to avoid false positives.
//
// Rules:
//   - a `var` declaration whose name is never read in its scope (or an enclosing closure) ->
//     "variable 'x' is assigned but never used".
//   - a bare expression statement whose value is non-None and has no useful side effect (a name,
//     literal, arithmetic, comparison, indexing, collection literal, f-string, or a call to a
//     locally-defined function that always returns a value) -> "result of expression is unused;
//     prefix with 'discard' to ignore it intentionally". `discard EXPR` suppresses this.
//   - a second `var` of the same name in one block -> "re-declared in this block" (block-scoped to
//     avoid flagging the legitimate `if: var x .. else: var x` pattern).
//   - a statement that can never run because the block already returned/threw/broke/continued ->
//     "unreachable code".
//   - `x = x` -> "self-assignment ... has no effect".
// (A repeated parameter name is a hard PARSE error now — see parser.hpp — not an analyzer warning.)

#include <string>
#include <vector>

#include "fum/unordered_map.hpp"
#include "fum/unordered_set.hpp"
#include "ast.hpp"
#include "common.hpp"

namespace kirito {

struct Warning {
    SourceSpan span;
    std::string message;
};

class Analyzer {
public:
    std::vector<Warning> analyze(const ast::Program& program) {
        // The module scope. Module-level names are exports (a .ki file's public API), so we never
        // flag them as unused — only function-local variables are checked. Functions push their own
        // (checked) scopes onto scopes_.
        pushScope(/*checkUnused=*/false);
        for (const auto& s : program.stmts) collectFunctions(*s);  // pre-pass: name -> function
        analyzeBlock(program.stmts);
        popScope();
        return std::move(warnings_);
    }

private:
    // Per-scope record of declared names: span of declaration + whether it has been read + whether
    // it is a parameter (parameters are never warned about — unused params are idiomatic).
    struct Decl { SourceSpan span; bool used = false; bool isParam = false; };
    struct Scope {
        fum::unordered_map<std::string, Decl> decls;
        fum::unordered_map<std::string, const ast::FunctionExpr*> funcs;
        bool checkUnused = true;  // false for module/class scopes (their names are exports/members)
    };
    std::vector<Scope> scopes_;
    std::vector<Warning> warnings_;
    fum::unordered_set<std::string> pendingUsed_;  // names read before their declaration (forward capture)
    int depth_ = 0;  // analyzeExpr recursion depth (bounded, anti-stack-overflow)
    struct DepthPop { int& d; ~DepthPop() { --d; } };

    void pushScope(bool checkUnused = true) { scopes_.emplace_back(); scopes_.back().checkUnused = checkUnused; }
    void popScope() {
        Scope& sc = scopes_.back();
        if (sc.checkUnused)
            for (auto& [name, d] : sc.decls)
                if (!d.used && !d.isParam && name != "self" && name[0] != '_')
                    warnings_.push_back({d.span, "variable '" + name + "' is assigned but never used"});
        scopes_.pop_back();
    }

    void declare(const std::string& name, SourceSpan span, bool isParam = false) {
        // A name read BEFORE its declaration in source order — a local captured by an EARLIER-defined
        // nested function, or mutual recursion (`isEven` referencing `isOdd` declared below) — calls
        // markUsed() before this declare(), so the mark would be lost and the local spuriously flagged
        // "assigned but never used" (F02-1). The resolver resolves such forward references by MEMBERSHIP;
        // mirror that here: if the name was used-before-declared, the new binding starts already used.
        bool wasForwardUsed = pendingUsed_.erase(name) > 0;
        scopes_.back().decls[name] = Decl{span, wasForwardUsed, isParam};
    }
    void markUsed(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->decls.find(name);
            if (f != it->decls.end()) { f->second.used = true; return; }
        }
        // Not yet declared in any live scope — remember it so a later declare() in this analysis marks
        // it used (forward capture / mutual recursion). A name that stays pending is a builtin/free
        // variable the resolver validates separately; leaving it here is harmless (worst case a later
        // same-named local is conservatively not warned — never a false POSITIVE).
        pendingUsed_.insert(name);
    }
    const ast::FunctionExpr* lookupFunc(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->funcs.find(name);
            if (f != it->funcs.end()) return f->second;
        }
        return nullptr;
    }

    // Pre-pass within a scope: record `var f = Function(...)` bindings so a forward/recursive call
    // can be recognized. Only direct function-literal initializers are tracked.
    void collectFunctions(const ast::Stmt& s) {
        if (const auto* v = dynamic_cast<const ast::VarDeclStmt*>(&s)) {
            if (v->names.size() == 1 && v->init)
                if (const auto* fn = dynamic_cast<const ast::FunctionExpr*>(v->init.get()))
                    scopes_.back().funcs[v->names[0]] = fn;
        }
    }

    // Does this function always end a value-producing return on every path? Conservative: true if
    // its return annotation is a non-None type, OR the final statement is `return <expr>`.
    static bool alwaysReturnsValue(const ast::FunctionExpr& fn) {
        if (!fn.returnAnnotation.empty()) return fn.returnAnnotation != "None";
        if (fn.body.empty()) return false;
        const auto* ret = dynamic_cast<const ast::ReturnStmt*>(fn.body.back().get());
        return ret && ret->value != nullptr;
    }

    // A statement that unconditionally transfers control, so anything after it in the same block is
    // unreachable. (Conservative: an `if` that returns on every branch is NOT treated as one — that
    // would need flow analysis and risk false positives.)
    static bool isTerminator(const ast::Stmt& s) {
        return dynamic_cast<const ast::ReturnStmt*>(&s) || dynamic_cast<const ast::ThrowStmt*>(&s)
            || dynamic_cast<const ast::BreakStmt*>(&s) || dynamic_cast<const ast::ContinueStmt*>(&s);
    }

    // --- statement walk -------------------------------------------------------------------------
    void analyzeBlock(const ast::Block& block) {
        fum::unordered_set<std::string> declaredHere;  // `var` names seen in THIS block
        bool terminated = false, unreachableWarned = false;
        for (const auto& s : block) {
            if (terminated && !unreachableWarned) {
                warnings_.push_back({s->span, "unreachable code (the block already returns/throws/"
                                              "breaks/continues before this)"});
                unreachableWarned = true;  // one warning per block is enough
            }
            if (const auto* v = dynamic_cast<const ast::VarDeclStmt*>(s.get()))
                for (const auto& name : v->names)
                    if (!declaredHere.insert(name).second)
                        warnings_.push_back({v->span,
                            "variable '" + name + "' is re-declared in this block"});
            analyzeStmt(*s);
            if (isTerminator(*s)) terminated = true;
        }
    }

    void analyzeStmt(const ast::Stmt& s) {
        if (const auto* e = dynamic_cast<const ast::ExprStmt*>(&s)) {
            warnIfUnusedResult(*e->expr);
            analyzeExpr(*e->expr);
        } else if (const auto* d = dynamic_cast<const ast::DiscardStmt*>(&s)) {
            analyzeExpr(*d->expr);  // discard explicitly accepts an ignored value
        } else if (const auto* v = dynamic_cast<const ast::VarDeclStmt*>(&s)) {
            analyzeExpr(*v->init);
            for (const auto& name : v->names) declare(name, v->span);
        } else if (const auto* a = dynamic_cast<const ast::AssignStmt*>(&s)) {
            // `x = x` is a no-op (the simple name-to-name case; member/index can have side effects
            // through custom protocols, so they are left alone).
            const auto* tgtName = dynamic_cast<const ast::NameExpr*>(a->target.get());
            const auto* valName = dynamic_cast<const ast::NameExpr*>(a->value.get());
            if (tgtName && valName && tgtName->name == valName->name)
                warnings_.push_back({a->span, "self-assignment of '" + tgtName->name + "' has no effect"});
            analyzeExpr(*a->value);
            analyzeAssignTarget(*a->target);
        } else if (const auto* i = dynamic_cast<const ast::IfStmt*>(&s)) {
            for (const auto& [cond, body] : i->branches) { analyzeExpr(*cond); analyzeBlock(body); }
            if (i->orelse) analyzeBlock(*i->orelse);
        } else if (const auto* w = dynamic_cast<const ast::WhileStmt*>(&s)) {
            analyzeExpr(*w->cond); analyzeBlock(w->body);
        } else if (const auto* f = dynamic_cast<const ast::ForStmt*>(&s)) {
            analyzeExpr(*f->iterable);
            for (const auto& name : f->vars) declare(name, f->span);  // loop vars count as used-ish
            for (const auto& name : f->vars) markUsed(name);          // don't warn on loop variables
            analyzeBlock(f->body);
        } else if (const auto* r = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            if (r->value) analyzeExpr(*r->value);
        } else if (const auto* t = dynamic_cast<const ast::TryStmt*>(&s)) {
            analyzeBlock(t->body);
            for (const auto& h : t->handlers) {
                if (h.type) analyzeExpr(*h.type);
                if (!h.name.empty()) { declare(h.name, t->span); markUsed(h.name); }
                analyzeBlock(h.body);
            }
            if (t->hasFinally) analyzeBlock(t->finallyBody);
        } else if (const auto* th = dynamic_cast<const ast::ThrowStmt*>(&s)) {
            analyzeExpr(*th->value);
        } else if (const auto* c = dynamic_cast<const ast::ClassStmt*>(&s)) {
            if (c->base) analyzeExpr(*c->base);
            declare(c->name, c->span); markUsed(c->name);  // a class name is a definition, not noise
            pushScope(/*checkUnused=*/false);  // class members are methods/attrs, never "unused"
            for (const auto& m : c->body) collectFunctions(*m);
            analyzeBlock(c->body);
            popScope();
        } else if (const auto* wi = dynamic_cast<const ast::WithStmt*>(&s)) {
            analyzeExpr(*wi->context);
            if (!wi->name.empty()) { declare(wi->name, wi->span); markUsed(wi->name); }
            analyzeBlock(wi->body);
        } else if (const auto* td = dynamic_cast<const ast::TodoStmt*>(&s)) {
            warnings_.push_back({td->span, td->message.empty()
                                               ? "todo: not yet implemented"
                                               : "todo: " + td->message});
        } else if (const auto* as = dynamic_cast<const ast::AssertStmt*>(&s)) {
            analyzeExpr(*as->cond);
            if (as->message) analyzeExpr(*as->message);
        } else if (const auto* sw = dynamic_cast<const ast::SwitchStmt*>(&s)) {
            analyzeExpr(*sw->subject);
            for (const auto& cl : sw->cases) {
                for (const auto& cv : cl.values) analyzeExpr(*cv);
                analyzeBlock(cl.body);
            }
            if (sw->hasDefault) analyzeBlock(sw->defaultBody);
        }
        // Break/Continue/Pass have nothing to analyze.
    }

    // An assignment target reads any names used to compute the location (index/member objects), and
    // for a bare name records nothing (it is a write, not a use).
    void analyzeAssignTarget(const ast::Expr& target) {
        ast::walkAssignTarget(target,
            [](const ast::NameExpr&) {},                   // a bare-name target is a write, not a use
            [&](const ast::Expr& e) { analyzeExpr(e); });  // index/member objects + keys are read
    }

    // --- "unused result" rule -------------------------------------------------------------------
    void warnIfUnusedResult(const ast::Expr& e) {
        if (producesUnusedValue(e))
            warnings_.push_back({e.span,
                "result of expression is unused; prefix with 'discard' to ignore it intentionally"});
    }

    // True when using `e` as a bare statement discards a value that the author very likely meant to
    // use, AND `e` has no side effect worth keeping. Calls are only flagged when we can see the
    // callee is a local function that always returns a value (native/unknown calls are left alone).
    bool producesUnusedValue(const ast::Expr& e) {
        switch (e.exprKind()) {
            case ast::ExprKind::Name:
            case ast::ExprKind::Index:
            case ast::ExprKind::Member:
            case ast::ExprKind::Tuple: {
                return true;
            } break;
            default: { } break;
        }
        if (const auto* lit = dynamic_cast<const ast::LiteralExpr*>(&e))
            return !std::holds_alternative<std::monostate>(lit->value);  // None literal is fine
        if (dynamic_cast<const ast::BinaryExpr*>(&e)) return true;
        if (dynamic_cast<const ast::UnaryExpr*>(&e)) return true;
        if (dynamic_cast<const ast::LogicalExpr*>(&e)) return true;
        if (dynamic_cast<const ast::ConditionalExpr*>(&e)) return true;
        if (dynamic_cast<const ast::ListLiteral*>(&e)) return true;
        if (dynamic_cast<const ast::SetLiteral*>(&e)) return true;
        if (dynamic_cast<const ast::DictLiteral*>(&e)) return true;
        if (dynamic_cast<const ast::FStringExpr*>(&e)) return true;
        if (const auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            if (const auto* n = dynamic_cast<const ast::NameExpr*>(call->callee.get()))
                if (const ast::FunctionExpr* fn = lookupFunc(n->name))
                    return alwaysReturnsValue(*fn);
        }
        return false;
    }

    // --- expression walk (use-tracking + nested functions) --------------------------------------
    void analyzeExpr(const ast::Expr& e) {
        // Bound recursion so a pathologically deep AST can't overflow the stack during analysis.
        // The analyzer is best-effort (non-fatal lint), so on an over-deep tree we simply stop
        // descending rather than throw — execution's own guards will report the real problem.
        if (depth_ > 2500) return;
        ++depth_;
        DepthPop pop{depth_};
        if (const auto* n = dynamic_cast<const ast::NameExpr*>(&e)) {
            markUsed(n->name);
        } else if (const auto* u = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            analyzeExpr(*u->operand);
        } else if (const auto* b = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            analyzeExpr(*b->lhs); analyzeExpr(*b->rhs);
        } else if (const auto* l = dynamic_cast<const ast::LogicalExpr*>(&e)) {
            analyzeExpr(*l->lhs); analyzeExpr(*l->rhs);
        } else if (const auto* cnd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
            analyzeExpr(*cnd->cond); analyzeExpr(*cnd->then); analyzeExpr(*cnd->orelse);
        } else if (const auto* c = dynamic_cast<const ast::CallExpr*>(&e)) {
            analyzeExpr(*c->callee);
            for (const auto& a : c->args) analyzeExpr(*a.value);
        } else if (const auto* m = dynamic_cast<const ast::MemberExpr*>(&e)) {
            analyzeExpr(*m->object);
        } else if (const auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            analyzeExpr(*ix->object);
            for (const auto& k : ix->indices) analyzeExpr(*k);
        } else if (const auto* sl = dynamic_cast<const ast::SliceExpr*>(&e)) {
            analyzeExpr(*sl->object);
            if (sl->start) analyzeExpr(*sl->start);
            if (sl->stop) analyzeExpr(*sl->stop);
            if (sl->step) analyzeExpr(*sl->step);
        } else if (const auto* lst = dynamic_cast<const ast::ListLiteral*>(&e)) {
            for (const auto& x : lst->elems) analyzeExpr(*x);
        } else if (const auto* st = dynamic_cast<const ast::SetLiteral*>(&e)) {
            for (const auto& x : st->elems) analyzeExpr(*x);
        } else if (const auto* dt = dynamic_cast<const ast::DictLiteral*>(&e)) {
            for (const auto& [k, v] : dt->entries) { analyzeExpr(*k); analyzeExpr(*v); }
        } else if (const auto* fs = dynamic_cast<const ast::FStringExpr*>(&e)) {
            for (const auto& p : fs->parts) if (p.isExpr) analyzeExpr(*p.expr);
        } else if (const auto* tup = dynamic_cast<const ast::TupleExpr*>(&e)) {
            for (const auto& x : tup->elems) analyzeExpr(*x);
        } else if (const auto* star = dynamic_cast<const ast::StarExpr*>(&e)) {
            analyzeExpr(*star->inner);
        } else if (const auto* fn = dynamic_cast<const ast::FunctionExpr*>(&e)) {
            analyzeFunction(*fn);
        }
        // LiteralExpr: nothing to do.
    }

    void analyzeFunction(const ast::FunctionExpr& fn) {
        pushScope();
        // Duplicate parameter names are now a hard PARSE error (parser.hpp), so the AST here never
        // holds one — no analyzer warning needed.
        for (const auto& p : fn.params) {
            if (p.defaultValue) analyzeExpr(*p.defaultValue);  // defaults evaluate in the outer-ish scope
            declare(p.name, fn.span, /*isParam=*/true);
        }
        for (const auto& s : fn.body) collectFunctions(*s);
        analyzeBlock(fn.body);
        popScope();
    }
};

// Convenience: analyze a parsed program and return formatted "line:col: warning: msg" strings.
inline std::vector<std::string> formatWarnings(const std::vector<Warning>& warnings,
                                               const std::string& file) {
    std::vector<std::string> out;
    for (const auto& w : warnings)
        out.push_back(file + ":" + std::to_string(w.span.line) + ":" + std::to_string(w.span.col) +
                      ": warning: " + w.message);
    return out;
}

}  // namespace kirito

#endif
