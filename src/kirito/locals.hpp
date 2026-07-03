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

inline void collectBlockDecls(const ast::Block& block, NameSet& out);

// Names a statement binds into the CURRENT scope. Descends into the sub-blocks that share this scope
// (if/while/for/try/with/switch) but NOT into nested function/class bodies (those are their own scopes).
inline void collectStmtDecls(const ast::Stmt& s, NameSet& out) {
    if (const auto* v = dynamic_cast<const ast::VarDeclStmt*>(&s)) {
        for (const auto& n : v->names) out.insert(n);
    } else if (const auto* f = dynamic_cast<const ast::ForStmt*>(&s)) {
        for (const auto& n : f->vars) out.insert(n);
        collectBlockDecls(f->body, out);
    } else if (const auto* c = dynamic_cast<const ast::ClassStmt*>(&s)) {
        out.insert(c->name);  // the class name binds in THIS scope; its body is a separate scope
    } else if (const auto* i = dynamic_cast<const ast::IfStmt*>(&s)) {
        for (const auto& [cond, b] : i->branches) collectBlockDecls(b, out);
        if (i->orelse) collectBlockDecls(*i->orelse, out);
    } else if (const auto* w = dynamic_cast<const ast::WhileStmt*>(&s)) {
        collectBlockDecls(w->body, out);
    } else if (const auto* t = dynamic_cast<const ast::TryStmt*>(&s)) {
        collectBlockDecls(t->body, out);
        for (const auto& h : t->handlers) { if (!h.name.empty()) out.insert(h.name); collectBlockDecls(h.body, out); }
        if (t->hasFinally) collectBlockDecls(t->finallyBody, out);
    } else if (const auto* wi = dynamic_cast<const ast::WithStmt*>(&s)) {
        if (!wi->name.empty()) out.insert(wi->name);
        collectBlockDecls(wi->body, out);
    } else if (const auto* sw = dynamic_cast<const ast::SwitchStmt*>(&s)) {
        for (const auto& cl : sw->cases) collectBlockDecls(cl.body, out);
        if (sw->hasDefault) collectBlockDecls(sw->defaultBody, out);
    }
    // ExprStmt/Discard/Assign/Return/Throw/Break/Continue/Pass/Todo/Assert bind nothing.
}

inline void collectBlockDecls(const ast::Block& block, NameSet& out) {
    for (const auto& s : block) collectStmtDecls(*s, out);
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
        for (const auto& p : fn.params) if (p.defaultValue) scanExpr(*p.defaultValue, inNested, nb);  // defaults: enclosing scope
        NameSet inner = nb;
        for (const auto& p : fn.params) inner.insert(p.name);
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

}  // namespace kirito

#endif
