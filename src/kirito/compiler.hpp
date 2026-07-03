#ifndef KIRITO_COMPILER_HPP
#define KIRITO_COMPILER_HPP

#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "fum/unordered_map.hpp"
#include "fum/unordered_set.hpp"
#include "ast.hpp"
#include "bytecode.hpp"
#include "builtins.hpp"   // floatToRoundtrip for exact switch-case float keys
#include "locals.hpp"     // collectBlockDecls + capturedLocals for slot-addressed locals
#include "vm.hpp"

namespace kirito {

// Compile a body once, caching the Proto on the VM. Defined below; forward-declared so the compiler
// can eagerly compile a nested class body (a genuine error there propagates as a KiritoError).
inline const Proto* protoForBody(KiritoVM& vm, const ast::Block& body, bool isFunction,
                                 const ast::FunctionExpr* fnDef = nullptr);

// Compiles a Block (a function body, the top-level program, or a class body) into a Proto — the AST's
// second visitor, alongside the parser. It emits stack-machine instructions that reuse the runtime's
// value/operator/call semantics verbatim. It handles every AST node; a genuine program error (a deep
// nest, an invalid assignment target, positional-after-keyword) is thrown as a KiritoError.
class Compiler : public ast::ExprVisitor, public ast::StmtVisitor {
public:
    Compiler(KiritoVM& vm, Proto& proto) : vm_(vm), proto_(proto) {}

    // Compile a whole body. isFunction picks the implicit tail: a function falls off the end
    // returning None; the top-level program returns its last expression value (the REPL echo).
    void compile(const ast::Block& body, bool isFunction, const ast::FunctionExpr* fnDef = nullptr) {
        if (fnDef) assignLocalSlots(*fnDef, body);  // slot-address this function's non-captured locals
        compileBlock(body);
        if (isFunction) { emit(Op::LoadNone); emit(Op::Return); }
        else { emit(Op::LoadResult); emit(Op::Return); }
        proto_.localCount = nextSlot_;  // includes $with hidden slots allocated during compilation
    }

    // Compile a single expression to a self-contained Proto (push the value, return it) — for a
    // parameter's default value, which is evaluated per call in the call scope.
    void compileSingleExpr(const ast::Expr& e) { compileExpr(e); emit(Op::Return); }

private:
    // --- emit / operand tables ---
    std::size_t emit(Op op, uint32_t a = 0, SourceSpan span = {}) {
        proto_.code.push_back(Instr{op, a, span});
        return proto_.code.size() - 1;
    }
    uint32_t here() const { return static_cast<uint32_t>(proto_.code.size()); }
    void patch(std::size_t at, uint32_t target) { proto_.code[at].a = target; }

    uint32_t addConst(Handle h) {
        std::string key = scalarConstKey(h);  // dedup repeated scalar literals -> one consts slot
        if (!key.empty()) {
            auto it = constDedup_.find(key);
            if (it != constDedup_.end()) return it->second;
        }
        proto_.consts.push_back(h);
        vm_.pushTemp(h);  // rooted while compiling; the VM pins it permanently once the Proto is kept
        uint32_t idx = static_cast<uint32_t>(proto_.consts.size() - 1);
        if (!key.empty()) constDedup_.emplace(key, idx);
        return idx;
    }
    // Canonical dedup key for a scalar constant. Empty for a non-scalar (never deduped). Floats key on
    // exact bits so -0.0/0.0 and distinct NaNs never collapse into one shared constant.
    std::string scalarConstKey(Handle h) const {
        const Object& o = vm_.arena().deref(h);
        switch (o.kind()) {
            case ValueKind::None: return "N";
            case ValueKind::Bool: return static_cast<const BoolVal&>(o).value() ? "B1" : "B0";
            case ValueKind::Integer: return "I" + std::to_string(static_cast<const IntVal&>(o).value());
            case ValueKind::Float:
                return "F" + std::to_string(std::bit_cast<uint64_t>(static_cast<const FloatVal&>(o).value()));
            case ValueKind::String: return "S" + static_cast<const StrVal&>(o).value();
            default: return std::string();
        }
    }
    uint32_t addName(const std::string& n) {
        for (std::size_t i = 0; i < proto_.names.size(); ++i)
            if (proto_.names[i] == n) return static_cast<uint32_t>(i);
        proto_.names.push_back(n);
        return static_cast<uint32_t>(proto_.names.size() - 1);
    }
    uint32_t addUnpack(uint32_t count, int starIndex) {
        proto_.unpacks.push_back(UnpackSpec{count, starIndex});
        return static_cast<uint32_t>(proto_.unpacks.size() - 1);
    }
    void emitCall0(SourceSpan span) {  // call the value on top of stack with no arguments
        proto_.calls.push_back(CallSpec{0, {}});
        emit(Op::Call, static_cast<uint32_t>(proto_.calls.size() - 1), span);
    }

    // --- slot-addressed locals -------------------------------------------------------------------
    // A function body's non-captured, non-parameter locals get a frame slot (direct array index at
    // run time) instead of a name lookup. Captured locals (referenced by a nested function/class) and
    // parameters stay name-based — they live in the scope's vars_, where closures and the call binder
    // find them. Module and class bodies never enable slots (slotsEnabled_ stays false): class bodies
    // rely on scope.locals() to harvest methods, and module scopes are dynamic.
    void assignLocalSlots(const ast::FunctionExpr& fn, const ast::Block& body) {
        slotsEnabled_ = true;
        NameSet captured = capturedLocals(fn.params, body);
        NameSet params;
        for (const auto& p : fn.params) params.insert(p.name);
        NameSet decls;
        collectBlockDecls(body, decls);
        for (const auto& name : decls)
            if (!captured.count(name) && !params.count(name)) defineSlot(name);
    }
    uint32_t defineSlot(const std::string& name) {
        uint32_t slot = nextSlot_++;
        slotOf_.emplace(name, slot);
        proto_.localNames.push_back(name);  // localNames[slot] == name
        return slot;
    }
    int slotOf(const std::string& name) const {
        auto it = slotOf_.find(name);
        return it == slotOf_.end() ? -1 : static_cast<int>(it->second);
    }
    // A compiler-generated hidden local ($withN): always slottable inside a function scope.
    void ensureHiddenSlot(const std::string& name) {
        if (slotsEnabled_ && slotOf(name) < 0) defineSlot(name);
    }
    // Emit a read / declare / rebind of a name, choosing the slot fast path when the name is slotted.
    void emitLoad(const std::string& name, SourceSpan span) {
        int s = slotOf(name);
        if (s >= 0) emit(Op::LoadLocal, static_cast<uint32_t>(s), span);
        else emit(Op::LoadName, addName(name), span);
    }
    void emitStore(const std::string& name, SourceSpan span) {
        int s = slotOf(name);
        if (s >= 0) emit(Op::StoreLocal, static_cast<uint32_t>(s), span);
        else emit(Op::StoreName, addName(name), span);
    }
    void emitAssign(const std::string& name, SourceSpan span) {
        int s = slotOf(name);
        if (s >= 0) emit(Op::AssignLocal, static_cast<uint32_t>(s), span);
        else emit(Op::AssignName, addName(name), span);
    }

    // The switch-dispatch key of a literal scalar, matching scalarSwitchKey() at run time, so the
    // compiler can detect duplicate `case` values (a compile-time error). nullopt for non-scalars.
    static std::optional<std::string> literalSwitchKey(const ast::LiteralExpr& lit) {
        if (std::holds_alternative<std::monostate>(lit.value)) return std::string("N");
        if (std::holds_alternative<bool>(lit.value))
            return std::string("B") + (std::get<bool>(lit.value) ? "1" : "0");
        if (std::holds_alternative<int64_t>(lit.value)) return "I" + std::to_string(std::get<int64_t>(lit.value));
        if (std::holds_alternative<double>(lit.value)) {
            double d = std::get<double>(lit.value);
            if (std::isnan(d)) return std::nullopt;            // NaN case matches nothing
            if (d == 0.0) d = 0.0;                             // -0.0 / 0.0 share a key
            return "F" + floatToRoundtrip(d);                  // EXACT key, must agree with scalarSwitchKey + ==
        }
        if (std::holds_alternative<std::string>(lit.value)) return "S" + std::get<std::string>(lit.value);
        return std::nullopt;
    }

    // --- recursion with a depth guard (matching the parser's nesting bound) so a pathologically deep
    // AST throws a clean error (with the node's span) instead of overflowing the compiler's stack. ---
    static constexpr int kMaxDepth = 3000;  // matches the parser/evaluator nesting bound
    struct DepthScope {
        int& d;
        DepthScope(int& dd, SourceSpan sp) : d(dd) {
            if (++d > kMaxDepth) { --d; throw KiritoError("expression too deeply nested to evaluate", sp); }
        }
        ~DepthScope() { --d; }
        DepthScope(const DepthScope&) = delete;
        DepthScope& operator=(const DepthScope&) = delete;
    };
    void compileExpr(const ast::Expr& e) { DepthScope g(depth_, e.span); e.accept(*this); }
    void compileStmt(const ast::Stmt& s) { DepthScope g(depth_, s.span); s.accept(*this); }
    void compileBlock(const ast::Block& b) { for (const auto& s : b) compileStmt(*s); }

    // --- statements ---
    void visit(const ast::ExprStmt& s) override { compileExpr(*s.expr); emit(Op::SetResult); }
    void visit(const ast::DiscardStmt& s) override { compileExpr(*s.expr); emit(Op::Pop); emit(Op::ClearResult); }

    void visit(const ast::VarDeclStmt& s) override {
        compileExpr(*s.init);
        if (s.names.size() == 1 && s.starIndex == -1) {
            emitStore(s.names[0], s.span);
        } else {
            emit(Op::Unpack, addUnpack(static_cast<uint32_t>(s.names.size()), s.starIndex), s.span);
            for (const auto& name : s.names) emitStore(name, s.span);  // first target on top
        }
        emit(Op::ClearResult);
    }

    void visit(const ast::AssignStmt& s) override {
        compileExpr(*s.value);
        if (s.target->exprKind() == ast::ExprKind::Tuple) {
            const auto& tup = static_cast<const ast::TupleExpr&>(*s.target);
            int starIndex = -1;
            for (std::size_t i = 0; i < tup.elems.size(); ++i)
                if (tup.elems[i]->exprKind() == ast::ExprKind::Star) {
                    if (starIndex != -1) throw KiritoError("two starred targets in assignment", s.span);
                    starIndex = static_cast<int>(i);
                }
            emit(Op::Unpack, addUnpack(static_cast<uint32_t>(tup.elems.size()), starIndex), s.span);
            for (const auto& elem : tup.elems) {  // forward order; slot_i is on top for target i
                const ast::Expr* tgt = elem.get();
                if (tgt->exprKind() == ast::ExprKind::Star) tgt = &*static_cast<const ast::StarExpr&>(*tgt).inner;
                compileAssignTarget(*tgt, s.span);
            }
        } else {
            compileAssignTarget(*s.target, s.span);
        }
        emit(Op::ClearResult);
    }

    // Store the value already on the stack into a single target (name, index, or member).
    void compileAssignTarget(const ast::Expr& target, SourceSpan span) {
        switch (target.exprKind()) {
            case ast::ExprKind::Name: {
                emitAssign(static_cast<const ast::NameExpr&>(target).name, span);
            } break;
            case ast::ExprKind::Index: {
                const auto& idx = static_cast<const ast::IndexExpr&>(target);
                compileExpr(*idx.object);
                for (const auto& ix : idx.indices) compileExpr(*ix);
                emit(Op::SetItem, static_cast<uint32_t>(idx.indices.size()), span);
            } break;
            case ast::ExprKind::Member: {
                const auto& mem = static_cast<const ast::MemberExpr&>(target);
                compileExpr(*mem.object);
                emit(Op::SetAttr, addName(mem.name), span);
            } break;
            default: { throw KiritoError("invalid assignment target", span); } break;
        }
    }

    void visit(const ast::IfStmt& s) override {
        std::vector<std::size_t> endJumps;
        for (const auto& [cond, body] : s.branches) {
            compileExpr(*cond);
            std::size_t next = emit(Op::PopJumpIfFalse, 0, s.span);
            compileBlock(body);
            endJumps.push_back(emit(Op::Jump));
            patch(next, here());
        }
        if (s.orelse) compileBlock(*s.orelse);
        for (std::size_t j : endJumps) patch(j, here());
        emit(Op::ClearResult);
    }

    void visit(const ast::WhileStmt& s) override {
        uint32_t start = here();
        compileExpr(*s.cond);
        std::size_t exit = emit(Op::PopJumpIfFalse, 0, s.span);
        frames_.push_back(CFrame{CFrame::Loop, 0, {}, {}, nullptr});
        compileBlock(s.body);
        emit(Op::Jump, start);
        uint32_t end = here();
        patch(exit, end);
        for (std::size_t j : frames_.back().breaks) patch(j, end);
        for (std::size_t j : frames_.back().continues) patch(j, start);
        frames_.pop_back();
        emit(Op::ClearResult);
    }

    void visit(const ast::ForStmt& s) override {
        compileExpr(*s.iterable);
        emit(Op::GetIter, 0, s.span);
        uint32_t top = here();
        std::size_t exit = emit(Op::ForIter, 0, s.span);  // exhausted -> pops the cursor, jumps to end
        if (s.vars.size() == 1 && s.starIndex == -1) {
            emitStore(s.vars[0], s.span);
        } else {
            emit(Op::Unpack, addUnpack(static_cast<uint32_t>(s.vars.size()), s.starIndex), s.span);
            for (const auto& v : s.vars) emitStore(v, s.span);  // first target on top
        }
        frames_.push_back(CFrame{CFrame::Loop, 1, {}, {}, nullptr});  // break must pop the live cursor
        compileBlock(s.body);
        emit(Op::Jump, top);
        uint32_t end = here();
        patch(exit, end);
        for (std::size_t j : frames_.back().breaks) patch(j, end);
        for (std::size_t j : frames_.back().continues) patch(j, top);
        frames_.pop_back();
        emit(Op::ClearResult);
    }

    void visit(const ast::BreakStmt&) override {
        std::size_t li = innermostLoop();
        unwindFramesAbove(li + 1);  // run finally/with cleanups between here and the loop
        for (int i = 0; i < frames_[li].unwind; ++i) emit(Op::Pop);  // drop the for-cursor
        frames_[li].breaks.push_back(emit(Op::Jump));
    }
    void visit(const ast::ContinueStmt&) override {
        std::size_t li = innermostLoop();
        unwindFramesAbove(li + 1);  // cleanups; the cursor stays (the loop's advance needs it)
        frames_[li].continues.push_back(emit(Op::Jump));
    }
    void visit(const ast::PassStmt&) override { emit(Op::ClearResult); }
    void visit(const ast::TodoStmt&) override { emit(Op::ClearResult); }

    void visit(const ast::AssertStmt& s) override {
        compileExpr(*s.cond);
        std::size_t ok = emit(Op::PopJumpIfTrue, 0, s.span);
        if (s.message) compileExpr(*s.message);
        else emit(Op::LoadConst, addConst(vm_.makeString("assertion failed")));
        emit(Op::Throw, 0, s.span);
        patch(ok, here());
        emit(Op::ClearResult);
    }

    void visit(const ast::ReturnStmt& s) override {
        if (s.value) compileExpr(*s.value);
        else emit(Op::LoadNone);
        unwindFramesAbove(0);  // run every enclosing finally/with cleanup before returning (value on stack)
        emit(Op::Return, 0, s.span);
    }

    void visit(const ast::ThrowStmt& s) override {
        compileExpr(*s.value);
        emit(Op::Throw, 0, s.span);
    }

    // `switch SUBJECT:` — no fallthrough, exact type+value matching (case 1 != case 1.0). When every
    // case value is a compile-time literal scalar (the documented + common form), it compiles to a
    // single SwitchDispatch against a hash table built once now — O(1) at run time, independent of the
    // case count. Otherwise (a non-literal case value) it falls back to a SwitchMatch comparison chain.
    // Duplicate literal case values are a compile error.
    void visit(const ast::SwitchStmt& s) override {
        // Duplicate literal case values are rejected — but the error is observable only when the
        // switch is REACHED (catchable, thrown at run time, like the old jump-table build), so on a
        // duplicate we evaluate the subject (for its side effects) and then throw at this position.
        {
            fum::unordered_set<std::string> seen;
            for (const auto& c : s.cases)
                for (const auto& valExpr : c.values)
                    if (const auto* lit = dynamic_cast<const ast::LiteralExpr*>(valExpr.get()))
                        if (auto key = literalSwitchKey(*lit); key && !seen.insert(*key).second) {
                            compileExpr(*s.subject);
                            emit(Op::Pop);
                            emit(Op::LoadConst, addConst(vm_.makeString("duplicate switch case value")));
                            emit(Op::Throw, 0, valExpr->span);
                            emit(Op::ClearResult);
                            return;
                        }
        }
        // Fast path: all case values are literal scalars -> one O(1) hash dispatch built at compile time.
        auto litKey = [](const ast::Expr& e) -> std::optional<std::string> {
            if (const auto* lit = dynamic_cast<const ast::LiteralExpr*>(&e)) return literalSwitchKey(*lit);
            return std::nullopt;
        };
        bool allLiteral = true;
        for (const auto& c : s.cases)
            for (const auto& v : c.values)
                if (!litKey(*v)) { allLiteral = false; break; }
        if (allLiteral) {
            compileExpr(*s.subject);
            std::size_t tblIdx = proto_.switches.size();
            proto_.switches.emplace_back();            // reserve the slot; the index is stable across nesting
            emit(Op::SwitchDispatch, static_cast<uint32_t>(tblIdx), s.span);  // pops subject, jumps to an arm
            SwitchTable table;
            std::vector<std::size_t> endJumps;
            for (std::size_t ci = 0; ci < s.cases.size(); ++ci) {
                uint32_t arm = here();
                for (const auto& v : s.cases[ci].values) table.targets[*litKey(*v)] = arm;
                compileBlock(s.cases[ci].body);
                endJumps.push_back(emit(Op::Jump));
            }
            table.defaultTarget = here();              // a missed key runs the default arm (or falls to end)
            if (s.hasDefault) compileBlock(s.defaultBody);
            for (std::size_t j : endJumps) patch(j, here());
            proto_.switches[tblIdx] = std::move(table);
            emit(Op::ClearResult);
            return;
        }
        compileExpr(*s.subject);                       // subject stays on the stack across the tests
        std::vector<std::vector<std::size_t>> caseJumps(s.cases.size());
        for (std::size_t ci = 0; ci < s.cases.size(); ++ci)
            for (const auto& valExpr : s.cases[ci].values) {
                emit(Op::Dup);
                compileExpr(*valExpr);
                emit(Op::SwitchMatch, 0, valExpr->span);
                caseJumps[ci].push_back(emit(Op::PopJumpIfTrue, 0, s.span));
            }
        std::vector<std::size_t> endJumps;
        emit(Op::Pop);                                 // no match: drop the subject, run default
        if (s.hasDefault) compileBlock(s.defaultBody);
        endJumps.push_back(emit(Op::Jump));
        for (std::size_t ci = 0; ci < s.cases.size(); ++ci) {
            uint32_t arm = here();
            for (std::size_t j : caseJumps[ci]) patch(j, arm);
            emit(Op::Pop);                             // arrived with the subject still on the stack
            compileBlock(s.cases[ci].body);
            endJumps.push_back(emit(Op::Jump));
        }
        for (std::size_t j : endJumps) patch(j, here());
        emit(Op::ClearResult);
    }

    // `class Name [(Base)]:` — eagerly compile the class body (caching it, surfacing any error in it
    // at compile time); push the base; BuildClass runs the body in a child scope and builds the class.
    void visit(const ast::ClassStmt& s) override {
        protoForBody(vm_, s.body, /*isFunction=*/true);  // compile + cache the body (errors propagate)
        if (s.base) compileExpr(*s.base);
        proto_.classes.push_back(&s);
        emit(Op::BuildClass, static_cast<uint32_t>(proto_.classes.size() - 1), s.span);
        emit(Op::ClearResult);
    }

    // `try: ... catch [T as e]: ... finally: ...`. Exception unwinding uses a runtime block stack
    // (SetupBlock/PopBlock + the executor's outer catch); break/continue/return crossing this try run
    // the finally via the frame-cleanup machinery. The finally body is duplicated per exit path
    // (normal / matched-handler / no-match / handler-exception) — small and simple beats clever.
    void visit(const ast::TryStmt& s) override {
        bool hasFin = s.hasFinally;
        bool hasHand = !s.handlers.empty();
        auto emitFinally = [this, &s, hasFin] { if (hasFin) compileBlock(s.finallyBody); };

        std::size_t finSetup = 0, exSetup = 0;
        if (hasFin) {  // outer block: catches exceptions in the body AND in handlers
            finSetup = emit(Op::SetupBlock, 0, s.span);
            frames_.push_back(CFrame{CFrame::Block, 0, {}, {}, emitFinally});
        }
        if (hasHand) {  // inner block: catches exceptions in the body, routes to the handlers
            exSetup = emit(Op::SetupBlock, 0, s.span);
            frames_.push_back(CFrame{CFrame::Block, 0, {}, {}, nullptr});
        }
        compileBlock(s.body);

        std::vector<std::size_t> endJumps;
        if (hasHand) { frames_.pop_back(); emit(Op::PopBlock); }  // body ok: drop the EXCEPT block
        if (hasFin) { frames_.pop_back(); emit(Op::PopBlock); emitFinally(); }  // run the normal-path finally
        endJumps.push_back(emit(Op::Jump));  // -> Lend

        if (hasHand) {
            patch(exSetup, here());  // Lhand: an exception unwound here, with the exception value on the stack
            if (hasFin) frames_.push_back(CFrame{CFrame::Block, 0, {}, {}, emitFinally});  // re-arm for handler bodies
            std::vector<std::size_t> toHandled;
            bool sawCatchAll = false;
            std::size_t pendingNext = SIZE_MAX;  // PopJumpIfFalse from the previous typed clause
            for (const auto& h : s.handlers) {
                if (pendingNext != SIZE_MAX) { patch(pendingNext, here()); pendingNext = SIZE_MAX; }
                if (h.type) {  // typed: match the exception against the class
                    emit(Op::Dup);
                    compileExpr(*h.type);
                    emit(Op::ExcMatch, 0, s.span);
                    pendingNext = emit(Op::PopJumpIfFalse, 0, s.span);
                } else {
                    sawCatchAll = true;
                }
                if (!h.name.empty()) emitStore(h.name, s.span);  // bind the exception
                else emit(Op::Pop);                              // or drop it
                compileBlock(h.body);
                toHandled.push_back(emit(Op::Jump));  // -> Lhandled
            }
            if (hasFin) frames_.pop_back();  // FINALLY frame no longer active beyond the handler bodies
            if (pendingNext != SIZE_MAX) patch(pendingNext, here());
            if (!sawCatchAll) {  // no handler matched: run finally, re-throw
                if (hasFin) { emit(Op::PopBlock); emitFinally(); }
                emit(Op::Reraise, 0, s.span);
            }
            uint32_t lhandled = here();
            for (std::size_t j : toHandled) patch(j, lhandled);
            if (hasFin) { emit(Op::PopBlock); emitFinally(); }  // a handler ran: run finally, continue
            endJumps.push_back(emit(Op::Jump));  // -> Lend
        }

        if (hasFin) {  // Lfin: an exception in a handler (or in a type expr) — finally then re-throw
            patch(finSetup, here());
            emitFinally();
            emit(Op::Reraise, 0, s.span);
        }
        for (std::size_t j : endJumps) patch(j, here());  // Lend
        // NB: unlike most statements, `try` does NOT clear the result — it carries the value of the
        // last expression in the executed body/handler, so the REPL echoes it.
    }

    // `with CTX as NAME:` — call CTX._enter_() (bound to NAME), run the body, and ALWAYS call
    // CTX._exit_() (on normal completion, break/continue/return, or exception). The manager is held in
    // a hidden local so the exit can reach it on every path, including after the stack is unwound.
    void visit(const ast::WithStmt& s) override {
        compileExpr(*s.context);
        std::string mgr = "$with" + std::to_string(withCounter_++);  // '$' can't appear in a user name
        ensureHiddenSlot(mgr);  // slot it inside a function scope (never captured); name-based elsewhere
        emitStore(mgr, s.span);
        emitLoad(mgr, s.span);
        emit(Op::GetAttr, addName("_enter_"), s.span);
        emitCall0(s.span);
        if (!s.name.empty()) emitStore(s.name, s.span);
        else emit(Op::Pop);
        auto emitExit = [this, mgr, span = s.span] {
            emitLoad(mgr, span);
            emit(Op::GetAttr, addName("_exit_"), span);
            emitCall0(span);
            emit(Op::Pop);  // discard the _exit_ return value
        };
        std::size_t setup = emit(Op::SetupBlock, 0, s.span);
        frames_.push_back(CFrame{CFrame::Block, 0, {}, {}, emitExit});
        compileBlock(s.body);
        frames_.pop_back();
        emit(Op::PopBlock);
        emitExit();  // normal-path exit
        std::size_t endJump = emit(Op::Jump);
        patch(setup, here());  // exception path: exit then re-throw
        emitExit();
        emit(Op::Reraise, 0, s.span);
        patch(endJump, here());
        emit(Op::ClearResult);
    }

    // --- expressions ---
    void visit(const ast::LiteralExpr& e) override {
        if (std::holds_alternative<int64_t>(e.value))
            emit(Op::LoadConst, addConst(vm_.makeInt(std::get<int64_t>(e.value))));
        else if (std::holds_alternative<double>(e.value))
            emit(Op::LoadConst, addConst(vm_.makeFloat(std::get<double>(e.value))));
        else if (std::holds_alternative<bool>(e.value))
            emit(Op::LoadConst, addConst(vm_.makeBool(std::get<bool>(e.value))));
        else if (std::holds_alternative<std::string>(e.value))
            emit(Op::LoadConst, addConst(vm_.makeString(std::get<std::string>(e.value))));
        else
            emit(Op::LoadNone);
    }

    void visit(const ast::NameExpr& e) override { emitLoad(e.name, e.span); }

    void visit(const ast::UnaryExpr& e) override {
        compileExpr(*e.operand);
        emit(Op::UnaryOp, static_cast<uint32_t>(e.op), e.span);
    }

    void visit(const ast::BinaryExpr& e) override {
        compileExpr(*e.lhs);
        compileExpr(*e.rhs);
        emit(Op::BinaryOp, static_cast<uint32_t>(e.op), e.span);
    }

    void visit(const ast::LogicalExpr& e) override {
        compileExpr(*e.lhs);
        std::size_t shortcut = emit(e.isAnd ? Op::JumpIfFalseOrPop : Op::JumpIfTrueOrPop, 0, e.span);
        compileExpr(*e.rhs);
        patch(shortcut, here());
    }

    void visit(const ast::ConditionalExpr& e) override {
        compileExpr(*e.cond);
        std::size_t toElse = emit(Op::PopJumpIfFalse, 0, e.span);
        compileExpr(*e.then);
        std::size_t toEnd = emit(Op::Jump);
        patch(toElse, here());
        compileExpr(*e.orelse);
        patch(toEnd, here());
    }

    void visit(const ast::FunctionExpr& e) override {
        proto_.funcs.push_back(&e);
        emit(Op::MakeFunction, static_cast<uint32_t>(proto_.funcs.size() - 1), e.span);
    }

    void visit(const ast::CallExpr& e) override {
        // A positional argument after a keyword argument is a (catchable) run-time error, like the old
        // evaluator: the callee and the arguments up to and including the offending one are evaluated,
        // then it throws. Detect it here and compile that throw instead of a normal call.
        {
            bool sawNamed = false;
            for (std::size_t i = 0; i < e.args.size(); ++i) {
                if (!e.args[i].name.empty()) { sawNamed = true; continue; }
                if (sawNamed) {
                    compileExpr(*e.callee);
                    for (std::size_t j = 0; j <= i; ++j) compileExpr(*e.args[j].value);
                    emit(Op::LoadConst,
                         addConst(vm_.makeString("positional argument follows keyword argument")));
                    emit(Op::Throw, 0, e.span);
                    return;
                }
            }
        }
        compileExpr(*e.callee);
        CallSpec spec;
        for (const auto& arg : e.args) {
            if (arg.name.empty()) ++spec.positional;
            else spec.names.push_back(arg.name);
        }
        for (const auto& arg : e.args)            // positional values, in source order
            if (arg.name.empty()) compileExpr(*arg.value);
        for (const auto& arg : e.args)            // then keyword values, in CallSpec.names order
            if (!arg.name.empty()) compileExpr(*arg.value);
        proto_.calls.push_back(std::move(spec));
        emit(Op::Call, static_cast<uint32_t>(proto_.calls.size() - 1), e.span);
    }

    void visit(const ast::MemberExpr& e) override {
        compileExpr(*e.object);
        emit(Op::GetAttr, addName(e.name), e.span);
    }

    void visit(const ast::IndexExpr& e) override {
        compileExpr(*e.object);
        for (const auto& ix : e.indices) compileExpr(*ix);
        emit(Op::GetItem, static_cast<uint32_t>(e.indices.size()), e.span);
    }

    void visit(const ast::SliceExpr& e) override {
        compileExpr(*e.object);
        if (e.start) compileExpr(*e.start); else emit(Op::LoadNone);
        if (e.stop) compileExpr(*e.stop); else emit(Op::LoadNone);
        if (e.step) compileExpr(*e.step); else emit(Op::LoadNone);
        emit(Op::GetSlice, 0, e.span);
    }

    void visit(const ast::ListLiteral& e) override {
        for (const auto& el : e.elems) compileExpr(*el);
        emit(Op::BuildList, static_cast<uint32_t>(e.elems.size()), e.span);
    }

    void visit(const ast::SetLiteral& e) override {
        for (const auto& el : e.elems) compileExpr(*el);
        emit(Op::BuildSet, static_cast<uint32_t>(e.elems.size()), e.span);
    }

    void visit(const ast::DictLiteral& e) override {
        for (const auto& [k, v] : e.entries) { compileExpr(*k); compileExpr(*v); }
        emit(Op::BuildDict, static_cast<uint32_t>(e.entries.size()), e.span);
    }

    void visit(const ast::TupleExpr& e) override {
        for (const auto& el : e.elems)
            if (el->exprKind() == ast::ExprKind::Star)
                throw KiritoError("starred expression is only valid as an assignment target", el->span);
        for (const auto& el : e.elems) compileExpr(*el);
        emit(Op::BuildPack, static_cast<uint32_t>(e.elems.size()), e.span);
    }

    void visit(const ast::StarExpr& e) override {
        throw KiritoError("starred expression is only valid as an assignment target", e.span);
    }

    void visit(const ast::FStringExpr& e) override {
        for (const auto& part : e.parts) {
            if (!part.isExpr) {
                emit(Op::LoadConst, addConst(vm_.makeString(part.literal)));
            } else {
                compileExpr(*part.expr);
                emit(Op::FormatValue, addName(part.spec), e.span);
            }
        }
        emit(Op::BuildString, static_cast<uint32_t>(e.parts.size()), e.span);
    }

    // The control-flow nesting stack. A Loop frame collects break/continue jumps to patch; a Block
    // frame is a runtime exception block (try/with) whose `cleanup` (a finally body, or a `with`'s
    // _exit_ call) must run when control leaves it normally OR via break/continue/return. break and
    // continue and return therefore "unwind" the frames between them and their target, emitting each
    // crossed Block frame's PopBlock + inline cleanup.
    struct CFrame {
        enum Kind { Loop, Block } kind;
        int unwind = 0;                              // Loop: operand slots to pop on break (for-cursor)
        std::vector<std::size_t> breaks, continues;  // Loop: jump sites to patch
        std::function<void()> cleanup;               // Block: emit the inline cleanup (empty for a bare except block)
    };

    std::size_t innermostLoop() {  // index of the nearest enclosing Loop frame (parser rejects break/continue outside one)
        for (std::size_t i = frames_.size(); i-- > 0;)
            if (frames_[i].kind == CFrame::Loop) return i;
        throw KiritoError("'break'/'continue' outside a loop");
    }

    // Emit PopBlock + inline cleanup for every frame above index `keep`, innermost first. Each frame is
    // removed before its own cleanup is emitted (so a cleanup that itself returns/breaks targets only
    // the still-active outer frames), then all are restored — leaving frames_ unchanged for sibling code.
    void unwindFramesAbove(std::size_t keep) {
        std::vector<CFrame> saved;
        while (frames_.size() > keep) {
            CFrame f = std::move(frames_.back());
            frames_.pop_back();
            if (f.kind == CFrame::Block) {
                emit(Op::PopBlock);
                if (f.cleanup) f.cleanup();
            }
            saved.push_back(std::move(f));
        }
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) frames_.push_back(std::move(*it));
    }

    KiritoVM& vm_;
    Proto& proto_;
    std::vector<CFrame> frames_;
    int withCounter_ = 0;  // unique hidden-local index per `with` (holds the context manager)
    int depth_ = 0;
    bool slotsEnabled_ = false;                      // true only when compiling a true function body
    fum::unordered_map<std::string, uint32_t> slotOf_;  // slotted local name -> frame slot index
    uint32_t nextSlot_ = 0;                          // next free slot (becomes proto_.localCount)
    fum::unordered_map<std::string, uint32_t> constDedup_;  // scalar const key -> consts index
};

// Compile a body/expression once and cache its Proto on the VM (keyed by the AST node's address). The
// shared skeleton — cache check, root-while-compiling, materialise, pin constants, store — lives in
// protoForImpl; the two public entries differ only in which compile step they run. The compiler
// handles every node, so this never fails to produce a Proto — a genuine program error (a deep nest,
// an invalid assignment target, ...) propagates out as a KiritoError, exactly as the parser's do.
template <typename CompileStep>
inline const Proto* protoForImpl(KiritoVM& vm, const void* key, CompileStep&& step) {
    if (vm.protoTried(key)) return vm.protoGet(key);
    RootScope rs(vm);  // roots the constants the compiler materialises until they are pinned
    auto p = std::make_unique<Proto>();
    Compiler c(vm, *p);
    step(c);
    for (Handle h : p->consts) vm.pinConst(h);  // survive past rs; live for the VM's lifetime
    const Proto* result = p.get();
    vm.protoPut(key, std::move(p));
    return result;
}

inline const Proto* protoForBody(KiritoVM& vm, const ast::Block& body, bool isFunction,
                                 const ast::FunctionExpr* fnDef) {
    return protoForImpl(vm, &body, [&](Compiler& c) { c.compile(body, isFunction, fnDef); });
}

// Compile a single expression (e.g. a parameter default) to its own Proto, cached by the expr's
// address. The Proto evaluates the expression and returns its value.
inline const Proto* protoForExpr(KiritoVM& vm, const ast::Expr& e) {
    return protoForImpl(vm, &e, [&](Compiler& c) { c.compileSingleExpr(e); });
}

}  // namespace kirito

#endif
