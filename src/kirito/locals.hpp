#ifndef KIRITO_LOCALS_HPP
#define KIRITO_LOCALS_HPP

// Shared compile-time scope analysis, the single source of truth used by BOTH the resolver (name
// validation) and the compiler (slot-addressed locals). Two questions are answered here:
//   * collectBlockDecls: which names does a scope bind? (var/for/catch/with/class/function-name)
//   * capturedLocals:    which of a function's locals are referenced from inside a nested function or
//                        class body — i.e. CAPTURED, and so must stay name-based (live in the scope's
//                        vars_) rather than being addressed by a frame slot.
// Only functions, the module/program, and class bodies introduce scopes; if/while/for/try/with/switch
// blocks share their enclosing scope, matching the runtime.

#include <string>
#include <vector>

#include "fum/unordered_set.hpp"
#include "ast.hpp"

namespace kirito {

using NameSet = fum::unordered_set<std::string>;

template <typename Sink> inline void forEachBlockDecl(const ast::Block& block, Sink&& sink);

// Visit, in source order, every name a statement binds into the CURRENT scope, calling sink(name)
// for each. Descends into the sub-blocks that share this scope (if/while/for/try/with/switch) but NOT
// into nested function/class bodies (those are their own scopes). The single traversal behind both
// collectBlockDecls (membership set) and collectBlockDeclsOrdered (deterministic slot order).
template <typename Sink>
inline void forEachStmtDecl(const ast::Stmt& s, Sink&& sink) {
    if (const auto* v = dynamic_cast<const ast::VarDeclStmt*>(&s)) {
        for (const auto& n : v->names) sink(n);
    } else if (const auto* f = dynamic_cast<const ast::ForStmt*>(&s)) {
        for (const auto& n : f->vars) sink(n);
        forEachBlockDecl(f->body, sink);
    } else if (const auto* c = dynamic_cast<const ast::ClassStmt*>(&s)) {
        sink(c->name);  // the class name binds in THIS scope; its body is a separate scope
    } else if (const auto* i = dynamic_cast<const ast::IfStmt*>(&s)) {
        for (const auto& [cond, b] : i->branches) forEachBlockDecl(b, sink);
        if (i->orelse) forEachBlockDecl(*i->orelse, sink);
    } else if (const auto* w = dynamic_cast<const ast::WhileStmt*>(&s)) {
        forEachBlockDecl(w->body, sink);
    } else if (const auto* t = dynamic_cast<const ast::TryStmt*>(&s)) {
        forEachBlockDecl(t->body, sink);
        for (const auto& h : t->handlers) { if (!h.name.empty()) sink(h.name); forEachBlockDecl(h.body, sink); }
        if (t->hasFinally) forEachBlockDecl(t->finallyBody, sink);
    } else if (const auto* wi = dynamic_cast<const ast::WithStmt*>(&s)) {
        if (!wi->name.empty()) sink(wi->name);
        forEachBlockDecl(wi->body, sink);
    } else if (const auto* sw = dynamic_cast<const ast::SwitchStmt*>(&s)) {
        for (const auto& cl : sw->cases) forEachBlockDecl(cl.body, sink);
        if (sw->hasDefault) forEachBlockDecl(sw->defaultBody, sink);
    }
    // ExprStmt/Discard/Assign/Return/Throw/Break/Continue/Pass/Todo/Assert bind nothing.
}

template <typename Sink>
inline void forEachBlockDecl(const ast::Block& block, Sink&& sink) {
    for (const auto& s : block) forEachStmtDecl(*s, sink);
}

inline void collectBlockDecls(const ast::Block& block, NameSet& out) {
    forEachBlockDecl(block, [&](const std::string& n) { out.insert(n); });
}

// The names a scope binds, in deterministic source-declaration order (deduped by first occurrence).
// Used to lay out a function scope's captured-local env slots identically in the resolver (which
// assigns each an index) and the runtime (which pre-declares them at frame entry).
inline std::vector<std::string> collectBlockDeclsOrdered(const ast::Block& block) {
    std::vector<std::string> out;
    NameSet seen;
    forEachBlockDecl(block, [&](const std::string& n) { if (seen.insert(n).second) out.push_back(n); });
    return out;
}

namespace detail {
// Walks a function body marking which of `localsF` are referenced from inside a nested scope. `nb` is
// the union of names bound by the nested scopes between F and the current point (they shadow F's
// locals); `inNested` is true once we are inside at least one nested scope, so a reference to one of
// F's still-visible locals is a genuine capture.
struct CaptureScan {
    const NameSet& localsF;
    NameSet& captured;

    void scanBlock(const ast::Block& b, bool inNested, const NameSet& nb) {
        for (const auto& s : b) scanStmt(*s, inNested, nb);
    }

    void enterFunction(const ast::FunctionExpr& fn, bool inNested, const NameSet& nb) {
        (void)inNested;  // enterFunction is only reached for a NESTED function literal, whose defaults
        // AND body both belong to its own lazily-evaluated closure — so both scan at inNested=true.
        // A parameter default is compiled to its own Proto and evaluated at fn's call time through fn's
        // closure (parent = the scope that defined fn), so a reference in a default to one of F's locals
        // is a genuine capture, exactly like a body reference (A03-1) — NOT the enclosing frame. Grow
        // `inner` over the earlier params first, so `Function(a, b = a)` binds `a` to fn's own param
        // (not a capture of an F-local named `a`).
        NameSet inner = nb;
        for (const auto& p : fn.params) {
            if (p.defaultValue) scanExpr(*p.defaultValue, /*inNested=*/true, inner);
            inner.insert(p.name);
        }
        collectBlockDecls(fn.body, inner);
        scanBlock(fn.body, /*inNested=*/true, inner);
    }
    void enterClass(const ast::ClassStmt& c, bool inNested, const NameSet& nb) {
        if (c.base) scanExpr(*c.base, inNested, nb);  // base: enclosing scope
        NameSet inner = nb;
        collectBlockDecls(c.body, inner);
        scanBlock(c.body, /*inNested=*/true, inner);
    }

    void scanExpr(const ast::Expr& e, bool inNested, const NameSet& nb) {
        if (const auto* n = dynamic_cast<const ast::NameExpr*>(&e)) {
            if (inNested && !nb.count(n->name) && localsF.count(n->name)) captured.insert(n->name);
        } else if (const auto* u = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            scanExpr(*u->operand, inNested, nb);
        } else if (const auto* b = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            scanExpr(*b->lhs, inNested, nb); scanExpr(*b->rhs, inNested, nb);
        } else if (const auto* l = dynamic_cast<const ast::LogicalExpr*>(&e)) {
            scanExpr(*l->lhs, inNested, nb); scanExpr(*l->rhs, inNested, nb);
        } else if (const auto* cn = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
            scanExpr(*cn->cond, inNested, nb); scanExpr(*cn->then, inNested, nb); scanExpr(*cn->orelse, inNested, nb);
        } else if (const auto* c = dynamic_cast<const ast::CallExpr*>(&e)) {
            scanExpr(*c->callee, inNested, nb);
            for (const auto& a : c->args) scanExpr(*a.value, inNested, nb);
        } else if (const auto* m = dynamic_cast<const ast::MemberExpr*>(&e)) {
            scanExpr(*m->object, inNested, nb);  // .name is a member, not a scope variable
        } else if (const auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            scanExpr(*ix->object, inNested, nb);
            for (const auto& k : ix->indices) scanExpr(*k, inNested, nb);
        } else if (const auto* sl = dynamic_cast<const ast::SliceExpr*>(&e)) {
            scanExpr(*sl->object, inNested, nb);
            if (sl->start) scanExpr(*sl->start, inNested, nb);
            if (sl->stop) scanExpr(*sl->stop, inNested, nb);
            if (sl->step) scanExpr(*sl->step, inNested, nb);
        } else if (const auto* lst = dynamic_cast<const ast::ListLiteral*>(&e)) {
            for (const auto& x : lst->elems) scanExpr(*x, inNested, nb);
        } else if (const auto* st = dynamic_cast<const ast::SetLiteral*>(&e)) {
            for (const auto& x : st->elems) scanExpr(*x, inNested, nb);
        } else if (const auto* dt = dynamic_cast<const ast::DictLiteral*>(&e)) {
            for (const auto& [k, v] : dt->entries) { scanExpr(*k, inNested, nb); scanExpr(*v, inNested, nb); }
        } else if (const auto* fs = dynamic_cast<const ast::FStringExpr*>(&e)) {
            for (const auto& p : fs->parts) if (p.isExpr) scanExpr(*p.expr, inNested, nb);
        } else if (const auto* tup = dynamic_cast<const ast::TupleExpr*>(&e)) {
            for (const auto& x : tup->elems) scanExpr(*x, inNested, nb);
        } else if (const auto* star = dynamic_cast<const ast::StarExpr*>(&e)) {
            scanExpr(*star->inner, inNested, nb);
        } else if (const auto* fn = dynamic_cast<const ast::FunctionExpr*>(&e)) {
            enterFunction(*fn, inNested, nb);
        }
        // LiteralExpr references nothing.
    }

    void scanStmt(const ast::Stmt& s, bool inNested, const NameSet& nb) {
        if (const auto* e = dynamic_cast<const ast::ExprStmt*>(&s)) scanExpr(*e->expr, inNested, nb);
        else if (const auto* d = dynamic_cast<const ast::DiscardStmt*>(&s)) scanExpr(*d->expr, inNested, nb);
        else if (const auto* v = dynamic_cast<const ast::VarDeclStmt*>(&s)) scanExpr(*v->init, inNested, nb);
        else if (const auto* a = dynamic_cast<const ast::AssignStmt*>(&s)) { scanExpr(*a->value, inNested, nb); scanExpr(*a->target, inNested, nb); }
        else if (const auto* i = dynamic_cast<const ast::IfStmt*>(&s)) {
            for (const auto& [cond, b] : i->branches) { scanExpr(*cond, inNested, nb); scanBlock(b, inNested, nb); }
            if (i->orelse) scanBlock(*i->orelse, inNested, nb);
        } else if (const auto* w = dynamic_cast<const ast::WhileStmt*>(&s)) { scanExpr(*w->cond, inNested, nb); scanBlock(w->body, inNested, nb); }
        else if (const auto* f = dynamic_cast<const ast::ForStmt*>(&s)) { scanExpr(*f->iterable, inNested, nb); scanBlock(f->body, inNested, nb); }
        else if (const auto* r = dynamic_cast<const ast::ReturnStmt*>(&s)) { if (r->value) scanExpr(*r->value, inNested, nb); }
        else if (const auto* t = dynamic_cast<const ast::TryStmt*>(&s)) {
            scanBlock(t->body, inNested, nb);
            for (const auto& h : t->handlers) { if (h.type) scanExpr(*h.type, inNested, nb); scanBlock(h.body, inNested, nb); }
            if (t->hasFinally) scanBlock(t->finallyBody, inNested, nb);
        } else if (const auto* th = dynamic_cast<const ast::ThrowStmt*>(&s)) scanExpr(*th->value, inNested, nb);
        else if (const auto* c = dynamic_cast<const ast::ClassStmt*>(&s)) enterClass(*c, inNested, nb);
        else if (const auto* wi = dynamic_cast<const ast::WithStmt*>(&s)) { scanExpr(*wi->context, inNested, nb); scanBlock(wi->body, inNested, nb); }
        else if (const auto* as = dynamic_cast<const ast::AssertStmt*>(&s)) { scanExpr(*as->cond, inNested, nb); if (as->message) scanExpr(*as->message, inNested, nb); }
        else if (const auto* sw = dynamic_cast<const ast::SwitchStmt*>(&s)) {
            scanExpr(*sw->subject, inNested, nb);
            for (const auto& cl : sw->cases) { for (const auto& cv : cl.values) scanExpr(*cv, inNested, nb); scanBlock(cl.body, inNested, nb); }
            if (sw->hasDefault) scanBlock(sw->defaultBody, inNested, nb);
        }
        // Break/Continue/Pass/Todo reference nothing.
    }
};
}  // namespace detail

// The function's locals (params + body decls) that are captured by a nested function/class body, and
// so must remain name-based. Over-approximating is safe (a captured local kept name-based is always
// correct); this analysis is precise enough to slot the common non-captured locals (loop counters,
// temporaries) while leaving genuinely-captured names alone.
inline NameSet capturedLocals(const std::vector<ast::Param>& params, const ast::Block& body) {
    NameSet localsF;
    for (const auto& p : params) localsF.insert(p.name);
    collectBlockDecls(body, localsF);
    NameSet captured;
    detail::CaptureScan scan{localsF, captured};
    scan.scanBlock(body, /*inNested=*/false, NameSet{});
    return captured;
}

namespace detail {
// Collects the names referenced FREE within a function/class subtree: every NameExpr whose name is
// bound by neither the subtree's own scope nor any nested scope enclosing the reference. The dual of
// CaptureScan — these are exactly the names a serialized function/class must carry from its defining
// scope (its closure). `bound` is the union of names bound by the subtree and every nested scope on
// the path to the current reference; a name not in it comes from the outside.
struct FreeVarScan {
    NameSet& free;

    void block(const ast::Block& b, const NameSet& bound) { for (const auto& s : b) stmt(*s, bound); }

    void func(const ast::FunctionExpr& fn, const NameSet& outer) {
        NameSet inner = outer;
        for (const auto& p : fn.params) {
            if (p.defaultValue) expr(*p.defaultValue, inner);  // a default sees the earlier params
            inner.insert(p.name);
        }
        collectBlockDecls(fn.body, inner);
        block(fn.body, inner);
    }
    void klass(const ast::ClassStmt& c, const NameSet& outer) {
        if (c.base) expr(*c.base, outer);            // the base resolves in the enclosing scope
        NameSet inner = outer;
        collectBlockDecls(c.body, inner);
        block(c.body, inner);
    }

    void expr(const ast::Expr& e, const NameSet& bound) {
        if (const auto* n = dynamic_cast<const ast::NameExpr*>(&e)) {
            if (!bound.count(n->name)) free.insert(n->name);
        } else if (const auto* u = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            expr(*u->operand, bound);
        } else if (const auto* b = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            expr(*b->lhs, bound); expr(*b->rhs, bound);
        } else if (const auto* l = dynamic_cast<const ast::LogicalExpr*>(&e)) {
            expr(*l->lhs, bound); expr(*l->rhs, bound);
        } else if (const auto* cn = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
            expr(*cn->cond, bound); expr(*cn->then, bound); expr(*cn->orelse, bound);
        } else if (const auto* c = dynamic_cast<const ast::CallExpr*>(&e)) {
            expr(*c->callee, bound);
            for (const auto& a : c->args) expr(*a.value, bound);
        } else if (const auto* m = dynamic_cast<const ast::MemberExpr*>(&e)) {
            expr(*m->object, bound);  // .name is a member, not a scope variable
        } else if (const auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            expr(*ix->object, bound);
            for (const auto& k : ix->indices) expr(*k, bound);
        } else if (const auto* sl = dynamic_cast<const ast::SliceExpr*>(&e)) {
            expr(*sl->object, bound);
            if (sl->start) expr(*sl->start, bound);
            if (sl->stop) expr(*sl->stop, bound);
            if (sl->step) expr(*sl->step, bound);
        } else if (const auto* lst = dynamic_cast<const ast::ListLiteral*>(&e)) {
            for (const auto& x : lst->elems) expr(*x, bound);
        } else if (const auto* st = dynamic_cast<const ast::SetLiteral*>(&e)) {
            for (const auto& x : st->elems) expr(*x, bound);
        } else if (const auto* dt = dynamic_cast<const ast::DictLiteral*>(&e)) {
            for (const auto& [k, v] : dt->entries) { expr(*k, bound); expr(*v, bound); }
        } else if (const auto* fs = dynamic_cast<const ast::FStringExpr*>(&e)) {
            for (const auto& p : fs->parts) if (p.isExpr) expr(*p.expr, bound);
        } else if (const auto* tup = dynamic_cast<const ast::TupleExpr*>(&e)) {
            for (const auto& x : tup->elems) expr(*x, bound);
        } else if (const auto* star = dynamic_cast<const ast::StarExpr*>(&e)) {
            expr(*star->inner, bound);
        } else if (const auto* fn = dynamic_cast<const ast::FunctionExpr*>(&e)) {
            func(*fn, bound);
        }
        // LiteralExpr references nothing.
    }

    void stmt(const ast::Stmt& s, const NameSet& bound) {
        if (const auto* e = dynamic_cast<const ast::ExprStmt*>(&s)) expr(*e->expr, bound);
        else if (const auto* d = dynamic_cast<const ast::DiscardStmt*>(&s)) expr(*d->expr, bound);
        else if (const auto* v = dynamic_cast<const ast::VarDeclStmt*>(&s)) expr(*v->init, bound);
        else if (const auto* a = dynamic_cast<const ast::AssignStmt*>(&s)) { expr(*a->value, bound); expr(*a->target, bound); }
        else if (const auto* i = dynamic_cast<const ast::IfStmt*>(&s)) {
            for (const auto& [cond, b] : i->branches) { expr(*cond, bound); block(b, bound); }
            if (i->orelse) block(*i->orelse, bound);
        } else if (const auto* w = dynamic_cast<const ast::WhileStmt*>(&s)) { expr(*w->cond, bound); block(w->body, bound); }
        else if (const auto* f = dynamic_cast<const ast::ForStmt*>(&s)) { expr(*f->iterable, bound); block(f->body, bound); }
        else if (const auto* r = dynamic_cast<const ast::ReturnStmt*>(&s)) { if (r->value) expr(*r->value, bound); }
        else if (const auto* t = dynamic_cast<const ast::TryStmt*>(&s)) {
            block(t->body, bound);
            for (const auto& h : t->handlers) { if (h.type) expr(*h.type, bound); block(h.body, bound); }
            if (t->hasFinally) block(t->finallyBody, bound);
        } else if (const auto* th = dynamic_cast<const ast::ThrowStmt*>(&s)) expr(*th->value, bound);
        else if (const auto* c = dynamic_cast<const ast::ClassStmt*>(&s)) klass(*c, bound);
        else if (const auto* wi = dynamic_cast<const ast::WithStmt*>(&s)) { expr(*wi->context, bound); block(wi->body, bound); }
        else if (const auto* as = dynamic_cast<const ast::AssertStmt*>(&s)) { expr(*as->cond, bound); if (as->message) expr(*as->message, bound); }
        else if (const auto* sw = dynamic_cast<const ast::SwitchStmt*>(&s)) {
            expr(*sw->subject, bound);
            for (const auto& cl : sw->cases) { for (const auto& cv : cl.values) expr(*cv, bound); block(cl.body, bound); }
            if (sw->hasDefault) block(sw->defaultBody, bound);
        }
        // Break/Continue/Pass/Todo reference nothing.
    }
};
}  // namespace detail

// All names a function literal references from its defining (closure) scope — the free-variable
// snapshot a serializer must carry so the function round-trips self-contained.
inline NameSet freeVariables(const ast::FunctionExpr& fn) {
    NameSet free;
    detail::FreeVarScan scan{free};
    scan.func(fn, NameSet{});
    return free;
}
// All names a class definition references from its defining scope (its base plus every method/attr
// body). Superset of eagerFreeVariables.
inline NameSet freeVariables(const ast::ClassStmt& c) {
    NameSet free;
    detail::FreeVarScan scan{free};
    scan.klass(c, NameSet{});
    return free;
}
// The subset of a class's free variables needed at DEFINITION time — referenced by the base expression
// or a non-method class-body statement (a class-variable initializer), which run when the class is
// built. A method (`var name = Function(...)`) is excluded: its body runs only when later called, so
// its free variables are "lazy" and may be wired after the class exists (this is what lets mutually
// recursive classes deserialize — see stdlib_serde.hpp). The base + eager refs must be real before the
// class is rebuilt; lazy ones can be placeholders.
inline NameSet eagerFreeVariables(const ast::ClassStmt& c) {
    NameSet free;
    detail::FreeVarScan scan{free};
    if (c.base) scan.expr(*c.base, NameSet{});
    NameSet bound;
    collectBlockDecls(c.body, bound);  // sibling class-body names are visible to each other's initializers
    for (const auto& s : c.body) {
        if (const auto* v = dynamic_cast<const ast::VarDeclStmt*>(s.get()))
            if (v->init && dynamic_cast<const ast::FunctionExpr*>(v->init.get())) continue;  // a method: lazy
        scan.stmt(*s, bound);
    }
    return free;
}

}  // namespace kirito

#endif
