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
#include "builtins.hpp"   // the core value types (Int/Float/Str/…) the emitted constants use
#include "locals.hpp"     // collectBlockDecls + capturedLocals for slot-addressed locals
#include "vm.hpp"

namespace kirito {

// Compile a body once, caching the Proto on the VM. Defined below; forward-declared so the compiler
// can eagerly compile a nested class body (a genuine error there propagates as a KiritoError).
inline const Proto* protoForBody(KiritoVM& vm, const ast::Block& body, bool isFunction,
                                 const ast::FunctionExpr* fnDef = nullptr);

// Runtime operator + switch-key helpers (defined later, in runtime.hpp — included by the umbrella AFTER
// compiler.hpp). Forward-declared here so the compiler can CONSTANT-FOLD a `switch` case label through
// the exact same semantics the VM uses at run time (so `case 3 + 4` keys identically to a runtime `3+4`).
inline Handle applyUnaryOp(KiritoVM& vm, UnOp op, Handle operand);
inline Handle applyBinaryOp(KiritoVM& vm, BinOp op, Handle lhs, Handle rhs);
inline std::optional<std::string> scalarSwitchKey(KiritoVM& vm, Handle h);
inline FormatSpec parseFormatSpec(const std::string& spec);  // pre-parse constant f-string specs

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
        if (fnDef) assignLocalSlots(*fnDef, body);        // a function: slot-address its non-captured locals
        else if (isFunction) collectClassEnvSlots(body);  // a class body (isFunction, no fnDef): index its names
        constMutable_ = kirito::mutatedOrRedeclared(body);  // names a `var f = fn` must NOT be, to be inlinable
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
    // Pre-parse a constant f-string format spec once, at compile time. A MALFORMED constant spec is
    // NOT a compile error (the docs classify it as a runtime error): store it deferred so FormatValue
    // re-parses it at run time and throws exactly as before.
    uint32_t addFormatSpec(const std::string& spec) {
        FormatSpec fs;
        try { fs = parseFormatSpec(spec); }
        catch (const KiritoError&) { fs = FormatSpec{}; fs.deferred = true; fs.raw = spec; fs.isEmpty = spec.empty(); }
        proto_.formatSpecs.push_back(std::move(fs));
        return static_cast<uint32_t>(proto_.formatSpecs.size() - 1);
    }
    uint32_t addUnpack(uint32_t count, int starIndex) {
        proto_.unpacks.push_back(UnpackSpec{count, starIndex});
        return static_cast<uint32_t>(proto_.unpacks.size() - 1);
    }
    // A resolver-annotated (depth, index) env reference -> a LoadVar/AssignVar operand.
    uint32_t addEnvVar(const ast::NameExpr& e) {
        proto_.envVars.push_back(EnvVarRef{static_cast<uint16_t>(e.envDepth),
                                           static_cast<uint32_t>(e.envIndex), e.name});
        return static_cast<uint32_t>(proto_.envVars.size() - 1);
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
        // Non-captured parameters get frame slots FIRST (slots 0..k-1, in parameter order) so the call
        // binder can place argument values into them by position. proto_.paramSlots[p] records each
        // param's slot (or -1 when the param is captured and must stay name-based in the scope env).
        proto_.paramSlots.reserve(fn.params.size());
        for (const auto& p : fn.params) {
            params.insert(p.name);
            if (!captured.count(p.name)) proto_.paramSlots.push_back(static_cast<int>(defineSlot(p.name)));
            else proto_.paramSlots.push_back(-1);
        }
        NameSet decls;
        collectBlockDecls(body, decls);
        for (const auto& name : decls)
            if (!captured.count(name) && !params.count(name)) defineSlot(name);
        // Captured non-param locals live in the scope's EnvValue (a nested closure reaches them), at
        // fixed indices AFTER the parameters. Record them in the SAME deterministic order the resolver
        // uses to assign their (depth, index), so the runtime pre-declares slot i for the name the
        // resolver addressed as index P+i. Read via LoadVar; declared via StoreName into the slot.
        for (const auto& name : collectBlockDeclsOrdered(body))
            if (captured.count(name) && !params.count(name)) proto_.envSlots.push_back(name);
    }
    // A class body is not slotted (its names are harvested by name into the class), but every top-level
    // name it binds lives in the class-scope EnvValue at a fixed index, so a method reading a sibling
    // method / class var by bare name compiles to a direct LoadVar. Record them in the SAME order the
    // resolver assigns their indices and the runtime pre-declares them (collectBlockDeclsOrdered).
    void collectClassEnvSlots(const ast::Block& body) {
        for (const auto& name : collectBlockDeclsOrdered(body)) proto_.envSlots.push_back(name);
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

    // --- function inlining (compile-time, hygienic; the transform is gated by vm_.inliningEnabled()) ---
    // v1 inlines a directly-called lambda literal that is capture-free (references only its params +
    // globals), a single `return EXPR`, flat (no nested function literal), unannotated, at exact
    // positional arity. Such a call becomes: evaluate each argument once into a fresh caller frame slot,
    // then emit the body expression with param names rebound to those slots — no call frame, no scope
    // allocation, identical result. Names inside the body resolve ONLY to those slots or to globals
    // (isolation by construction); caller locals are invisible to the body and vice versa. This is the
    // shared primitive Part C (combinator lowering) reuses. Multi-statement / capturing / annotated
    // bodies are simply not v1 candidates and compile as a normal call (correct, not a fallback).
    int inlineCounter_ = 0;
    // One rebinding for a name inside an inlined body, consulted by visit(NameExpr): a PARAMETER reads a
    // unique hidden binding ($inlN_x / $cmbN_x) holding its once-evaluated argument (frame slot in a
    // function, module binding at top level; `$`-prefixed so never a module export); a CAPTURE reads the
    // enclosing variable IN PLACE via LoadVar(capDepth, capIndex) — the call site is exactly one scope
    // shallower than the lambda body, so a body capture at (envDepth, envIndex) is (envDepth-1, envIndex)
    // here. Reading a capture in place (not lifting once) matches closure-by-reference semantics and is
    // correct even when the surrounding loop mutates it between elements; the body itself cannot mutate a
    // captured variable (write-through closures are forbidden), so repeated reads are consistent.
    struct InlineBind { std::string name; std::string hidden; bool capture; uint16_t capDepth; uint32_t capIndex; };
    const std::vector<InlineBind>* inlineRebind_ = nullptr;
    NameSet constMutable_;   // names rebound/redeclared in this body -> NOT an immutable const-fn binding
    fum::unordered_map<std::string, const ast::FunctionExpr*> constLambdaDefs_;  // f -> its literal (inlinable)
    std::vector<const ast::FunctionExpr*> inlineStack_;   // lambdas currently being inlined (cycle guard)
    static constexpr std::size_t kMaxInlineDepth = 8;     // backstop vs mutual recursion / code-size blowup

    bool inlineBlocked(const ast::FunctionExpr* fn) const {
        if (inlineStack_.size() >= kMaxInlineDepth) return true;
        for (const ast::FunctionExpr* f : inlineStack_) if (f == fn) return true;  // already inlining it -> cycle
        return false;
    }

    uint32_t addEnvVarRaw(uint16_t depth, uint32_t index, const std::string& name) {
        proto_.envVars.push_back(EnvVarRef{depth, index, name});
        return static_cast<uint32_t>(proto_.envVars.size() - 1);
    }

    // The body expression of an inline-eligible lambda, or nullptr, filling `caps` with the captured
    // NameExprs (enclosing variables the body reads). Eligible = a single `return EXPR`, flat (no nested
    // function literal), unannotated, no parameter defaults/varargs, and every non-param reference is a
    // genuine global or an env-indexed capture (inlineBodyScan). Arity/context are the CALLER's concern.
    const ast::Expr* lambdaInlineBody(const ast::FunctionExpr& fn, std::vector<const ast::NameExpr*>& caps) const {
        if (!vm_.inliningEnabled() || !fn.returnAnnotation.empty()) return nullptr;
        for (const auto& p : fn.params)
            if (!p.annotation.empty() || p.defaultValue) return nullptr;
        if (fn.body.size() != 1) return nullptr;
        const auto* r = dynamic_cast<const ast::ReturnStmt*>(fn.body[0].get());
        if (!r || !r->value) return nullptr;
        NameSet params;
        for (const auto& p : fn.params) params.insert(p.name);
        if (!kirito::inlineBodyScan(*r->value, params, caps)) return nullptr;
        return r->value.get();
    }

    // Resolve a call's callee to an inline-eligible lambda — a literal `(Function...)(...)` or a NameExpr
    // bound to an immutable const-fn (var f = <literal>, never rebound). Returns the body + sets fnOut +
    // fills caps; nullptr if not a candidate. Recursion needs no special guard: a self/peer-referencing
    // body captures a non-global name; that capture reads a stale enclosing slot only if... in fact a
    // recursive const-fn `f` references `f`, which is env-indexed at the DEFINING scope and would be
    // relocated correctly — but `f` is not yet assigned when inlined, so we exclude a callee that
    // captures its own binding name below.
    const ast::Expr* inlineBodyIfCandidate(const ast::CallExpr& e, const ast::FunctionExpr*& fnOut,
                                           std::vector<const ast::NameExpr*>& caps) const {
        const ast::FunctionExpr* fn = dynamic_cast<const ast::FunctionExpr*>(e.callee.get());
        const std::string* boundName = nullptr;
        if (!fn)
            if (const auto* nm = dynamic_cast<const ast::NameExpr*>(e.callee.get())) {
                auto it = constLambdaDefs_.find(nm->name);
                if (it != constLambdaDefs_.end()) { fn = it->second; boundName = &nm->name; }
            }
        if (!fn || e.args.size() != fn->params.size() || inlineBlocked(fn)) return nullptr;  // arity + cycle guard
        for (const auto& a : e.args)
            if (!a.name.empty() || dynamic_cast<const ast::StarExpr*>(a.value.get())) return nullptr;  // no kw/starred
        const ast::Expr* body = lambdaInlineBody(*fn, caps);
        if (!body) return nullptr;
        // A const-fn that captures its OWN binding name is recursive — not inlinable (the binding is not
        // yet defined when we splice the body, and inlining would loop). Fall back to a normal call.
        if (boundName)
            for (const ast::NameExpr* c : caps) if (c->name == *boundName) return nullptr;
        fnOut = fn;
        return body;
    }

    // Record `var f = <inline-eligible function literal>` as an immutable const-fn binding, so later
    // calls `f(x)` in this scope inline. Only when f is never rebound/redeclared (constMutable_).
    void trackConstLambda(const ast::VarDeclStmt& s) {
        if (s.names.size() != 1 || s.starIndex != -1 || s.forceUnpack) return;
        const auto* fn = dynamic_cast<const ast::FunctionExpr*>(s.init.get());
        if (!fn || constMutable_.count(s.names[0])) return;
        std::vector<const ast::NameExpr*> caps;
        if (lambdaInlineBody(*fn, caps)) constLambdaDefs_[s.names[0]] = fn;
    }

    // Combinator lowering (Part C): a lambda literal passed as `map`/`filter`'s callback, inline-eligible
    // and taking exactly one parameter (the element). Guarded so a user shadow of `map`/`filter` (which
    // resolves to a local, not the builtin slot) is never lowered. Fills caps like lambdaInlineBody.
    enum class Combinator { None, Map, Filter };
    Combinator recognizeForCombinator(const ast::Expr& iterable, const ast::FunctionExpr*& lambdaOut,
                                       const ast::Expr*& srcOut, const ast::Expr*& bodyOut,
                                       std::vector<const ast::NameExpr*>& caps) const {
        if (!vm_.inliningEnabled()) return Combinator::None;
        const auto* call = dynamic_cast<const ast::CallExpr*>(&iterable);
        if (!call || call->args.size() != 2) return Combinator::None;
        for (const auto& a : call->args)
            if (!a.name.empty() || dynamic_cast<const ast::StarExpr*>(a.value.get())) return Combinator::None;
        const auto* callee = dynamic_cast<const ast::NameExpr*>(call->callee.get());
        if (!callee || callee->builtinSlot < 0) return Combinator::None;   // must be the BUILTIN, not a shadow
        Combinator which = callee->name == "map" ? Combinator::Map
                         : callee->name == "filter" ? Combinator::Filter : Combinator::None;
        if (which == Combinator::None) return Combinator::None;
        const auto* lam = dynamic_cast<const ast::FunctionExpr*>(call->args[0].value.get());
        if (!lam || lam->params.size() != 1 || inlineBlocked(lam)) return Combinator::None;   // 1-arg + cycle guard
        const ast::Expr* body = lambdaInlineBody(*lam, caps);
        if (!body) return Combinator::None;
        lambdaOut = lam; srcOut = call->args[1].value.get(); bodyOut = body;
        return which;
    }

    // Append capture rebindings (deduped by name): a body capture at (envDepth, envIndex) reads the
    // enclosing variable in place at the call site via LoadVar(envDepth-1, envIndex).
    void addCaptureBinds(const std::vector<const ast::NameExpr*>& caps, std::vector<InlineBind>& binds) {
        for (const ast::NameExpr* c : caps) {
            bool seen = false;
            for (const auto& b : binds) if (b.name == c->name) { seen = true; break; }
            if (seen) continue;
            binds.push_back(InlineBind{c->name, "", true, static_cast<uint16_t>(c->envDepth - 1),
                                       static_cast<uint32_t>(c->envIndex)});
        }
    }

    void emitInlinedCall(const ast::CallExpr& e, const ast::FunctionExpr& fn, const ast::Expr& body,
                         const std::vector<const ast::NameExpr*>& caps) {
        int n = inlineCounter_++;
        std::vector<InlineBind> binds;
        binds.reserve(fn.params.size() + caps.size());
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            compileExpr(*e.args[i].value);   // evaluate the argument ONCE, in order, in the caller context
            std::string hidden = "$inl" + std::to_string(n) + "_" + fn.params[i].name;
            ensureHiddenSlot(hidden);        // frame slot inside a function; a module binding at top level
            emitStore(hidden, e.span);
            binds.push_back(InlineBind{fn.params[i].name, hidden, false, 0, 0});
        }
        addCaptureBinds(caps, binds);
        const auto* prev = inlineRebind_;   // save/restore supports nested inlines
        inlineRebind_ = &binds;
        inlineStack_.push_back(&fn);        // cycle guard: a nested inline of the same lambda falls back
        compileExpr(body);                  // the body value is left on the stack = the call's result
        inlineStack_.pop_back();
        inlineRebind_ = prev;
    }

    // Fused `for VAR in map/filter(LAMBDA, SRC): BODY`: iterate SRC directly and inline the callback per
    // element — no combinator view, no per-element call. Mirrors visit(ForStmt)'s cursor/frame handling
    // so break/continue and the operand-stack height behave exactly as in a normal for-loop.
    void emitFusedForCombinator(const ast::ForStmt& s, Combinator which, const ast::FunctionExpr& lam,
                                const ast::Expr& src, const ast::Expr& body,
                                const std::vector<const ast::NameExpr*>& caps) {
        int n = inlineCounter_++;
        compileExpr(src);
        emit(Op::GetIter, 0, s.span);
        uint32_t top = here();
        std::size_t exit = emit(Op::ForIter, 0, s.span);   // element on stack, or jump to end when exhausted
        std::string elem = "$cmb" + std::to_string(n) + "_" + lam.params[0].name;
        ensureHiddenSlot(elem);
        emitStore(elem, s.span);                           // element -> hidden binding (the callback's param)
        std::vector<InlineBind> binds{InlineBind{lam.params[0].name, elem, false, 0, 0}};
        addCaptureBinds(caps, binds);                      // captures read in place per element (LoadVar)
        const auto* prev = inlineRebind_;
        inlineRebind_ = &binds;
        inlineStack_.push_back(&lam);
        compileExpr(body);                                 // inlined callback body -> value on the stack
        inlineStack_.pop_back();
        inlineRebind_ = prev;
        std::size_t skip = 0;
        if (which == Combinator::Filter) {
            skip = emit(Op::PopJumpIfFalse, 0, s.span);    // predicate false -> resume with the next element
            emitLoad(elem, s.span);                        // predicate true -> the element is the loop value
        }
        emitStore(s.vars[0], s.span);                      // map: the mapped value; filter: the element
        frames_.push_back(CFrame{CFrame::Loop, 1, {}, {}, nullptr});   // cursor live: break must pop it
        compileBlock(s.body);
        emit(Op::Jump, top);
        if (which == Combinator::Filter) patch(skip, top);
        uint32_t end = here();
        patch(exit, end);
        for (std::size_t j : frames_.back().breaks) patch(j, end);
        for (std::size_t j : frames_.back().continues) patch(j, top);
        frames_.pop_back();
        emit(Op::ClearResult);
    }

    // Compile-time fold of a CONSTANT `switch` case-label expression to its value, reusing the VM's own
    // operators so `case 3 + 4` / `case -1` / `case "a" + "b"` produce a value bit-identical to what the
    // same expression yields at run time. Returns nullopt if the label reads runtime state (a name /
    // call / index / member / collection literal) — that is a compile error at the call site. A constant
    // that ERRORS while folding (e.g. `case 1 / 0`) throws here, which is exactly the desired outcome.
    std::optional<Handle> foldConstValue(const ast::Expr& e) {
        if (const auto* lit = dynamic_cast<const ast::LiteralExpr*>(&e)) {
            if (std::holds_alternative<std::monostate>(lit->value)) return vm_.none();
            if (std::holds_alternative<bool>(lit->value)) return vm_.makeBool(std::get<bool>(lit->value));
            if (std::holds_alternative<int64_t>(lit->value)) return vm_.makeInt(std::get<int64_t>(lit->value));
            if (std::holds_alternative<double>(lit->value)) return vm_.makeFloat(std::get<double>(lit->value));
            if (std::holds_alternative<std::string>(lit->value)) return vm_.makeString(std::get<std::string>(lit->value));
            return std::nullopt;
        }
        if (const auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            auto oh = foldConstValue(*un->operand);
            if (!oh) return std::nullopt;
            RootScope rs(vm_); rs.add(*oh);                    // keep the operand across the allocating op
            return applyUnaryOp(vm_, un->op, *oh);
        }
        if (const auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            auto lh = foldConstValue(*bin->lhs);
            if (!lh) return std::nullopt;
            RootScope rs(vm_); rs.add(*lh);
            auto rh = foldConstValue(*bin->rhs);
            if (!rh) return std::nullopt;
            rs.add(*rh);
            return applyBinaryOp(vm_, bin->op, *lh, *rh);
        }
        if (const auto* lg = dynamic_cast<const ast::LogicalExpr*>(&e)) {  // and/or, short-circuit
            auto lh = foldConstValue(*lg->lhs);
            if (!lh) return std::nullopt;
            bool lt = vm_.arena().deref(*lh).truthy();
            if (lg->isAnd ? !lt : lt) return lh;               // `and` keeps a falsy left; `or` a truthy left
            return foldConstValue(*lg->rhs);
        }
        return std::nullopt;                                   // a name/call/index/member/… reads state
    }
    // The switch-dispatch key of a constant case label, or nullopt if it is non-constant or the folded
    // value is not a hashable scalar. Uses scalarSwitchKey so the key agrees with the subject's at run time.
    std::optional<std::string> foldConstKey(const ast::Expr& e) {
        auto h = foldConstValue(e);
        if (!h) return std::nullopt;
        return scalarSwitchKey(vm_, *h);
    }

    // Constant folding for general expressions: if `e` is a constant scalar expression, emit one
    // LoadConst and return true. A fold that would THROW (e.g. `1 / 0`) is left for runtime, so
    // try/catch and dead-code-not-reached semantics are preserved; an oversized folded String is also
    // left un-folded so the const pool can't be bloated (mirrors kMaxRepeat's intent).
    static constexpr std::size_t kMaxFoldString = 4096;
    bool tryEmitFolded(const ast::Expr& e) {
        std::optional<Handle> h;
        try { h = foldConstValue(e); }
        catch (const KiritoError&) { return false; }   // keep the error at runtime (catchable)
        if (!h) return false;
        // Size-check the folded value under a RootScope that pins `h` across the deref, then CLOSE that
        // scope BEFORE addConst. addConst's own pushTemp is what keeps the constant rooted for the rest
        // of compilation, and ~RootScope truncates the SHARED tempRoots_ stack (popTempTo) — so calling
        // addConst inside the scope would let ~RootScope pop addConst's root the instant this returns,
        // leaving a non-interned folded const (large Int/Float/String) unrooted until a later compile-
        // time GC sweeps it → runtime "dangling handle (stale generation)". No arena allocation happens
        // between the scope closing and addConst's pushTemp, so `h` stays live across the gap.
        {
            RootScope rs(vm_); rs.add(*h);
            const Object& v = vm_.arena().deref(*h);
            if (v.kind() == ValueKind::String && static_cast<const StrVal&>(v).value().size() > kMaxFoldString)
                return false;
        }
        emit(Op::LoadConst, addConst(*h), e.span);
        return true;
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
        if (vm_.inliningEnabled()) trackConstLambda(s);   // note an immutable `var f = fn` for later inlining
        compileExpr(*s.init);
        if (s.names.size() == 1 && s.starIndex == -1 && !s.forceUnpack) {
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
                const auto& n = static_cast<const ast::NameExpr&>(target);
                if (n.envIndex >= 0) emit(Op::AssignVar, addEnvVar(n), span);  // rebind an indexed env slot
                else emitAssign(n.name, span);
            } break;
            case ast::ExprKind::Index: {
                const auto& idx = static_cast<const ast::IndexExpr&>(target);
                compileExpr(*idx.object);
                for (const auto& ix : idx.indices) compileExpr(*ix);   // a slice element compiles to MakeSlice
                emit(Op::SetItem, static_cast<uint32_t>(idx.indices.size()), span);
            } break;
            case ast::ExprKind::Slice: {
                // Single-axis slice assignment `obj[a:b:c] = v` -> setItem with ONE Slice key.
                const auto& sl = static_cast<const ast::SliceExpr&>(target);
                compileExpr(*sl.object);
                if (sl.start) compileExpr(*sl.start); else emit(Op::LoadNone);
                if (sl.stop) compileExpr(*sl.stop); else emit(Op::LoadNone);
                if (sl.step) compileExpr(*sl.step); else emit(Op::LoadNone);
                emit(Op::MakeSlice);
                emit(Op::SetItem, 1, span);
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
        // Part C: `for VAR in map/filter(LAMBDA, SRC): BODY` fuses into a single loop that inlines the
        // callback per element — no MapVal/FilterVal, no per-element call. Only the single-target form is
        // lowered; anything else (or a shadowed builtin / non-lambda callback) uses the normal path.
        if (s.vars.size() == 1 && s.starIndex == -1 && !s.forceUnpack) {
            const ast::FunctionExpr* lam = nullptr; const ast::Expr* src = nullptr; const ast::Expr* body = nullptr;
            std::vector<const ast::NameExpr*> caps;
            Combinator which = recognizeForCombinator(*s.iterable, lam, src, body, caps);
            if (which != Combinator::None) { emitFusedForCombinator(s, which, *lam, *src, *body, caps); return; }
        }
        compileExpr(*s.iterable);
        emit(Op::GetIter, 0, s.span);
        uint32_t top = here();
        std::size_t exit = emit(Op::ForIter, 0, s.span);  // exhausted -> pops the cursor, jumps to end
        if (s.vars.size() == 1 && s.starIndex == -1 && !s.forceUnpack) {
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
        // If this return crosses any try-finally / with whose cleanup runs here, park the return value
        // in a hidden local so those cleanups run at a CLEAN operand height and are reloaded after —
        // exactly as emitFinallyExc parks the in-flight exception. Otherwise a break/continue inside a
        // crossed finally would Pop the return value (top of stack) instead of a loop cursor, orphaning
        // the cursor and corrupting an enclosing loop's iteration (A04-1).
        bool crossesCleanup = false;
        for (const auto& f : frames_)
            if (f.kind == CFrame::Block && f.cleanup) { crossesCleanup = true; break; }
        if (crossesCleanup) {
            std::string retName = "$ret" + std::to_string(retCounter_++);
            ensureHiddenSlot(retName);
            emitStore(retName, s.span);
            unwindFramesAbove(0);
            emitLoad(retName, s.span);
        } else {
            unwindFramesAbove(0);  // no cleanup to run at height — the value can ride the stack
        }
        emit(Op::Return, 0, s.span);
    }

    void visit(const ast::ThrowStmt& s) override {
        compileExpr(*s.value);
        emit(Op::Throw, 0, s.span);
    }

    // `switch SUBJECT:` — no fallthrough, exact type+value matching (`case 1` != `case 1.0`). Every case
    // label must be a COMPILE-TIME CONSTANT SCALAR: a literal, or an expression over literals that folds
    // to a scalar (`case 3 + 4`, `case -1`, `case "a" + "b"`). A label that reads runtime state (a
    // variable, a call, an index/member) is a compile error; so is a duplicate value and a non-scalar
    // constant. Because every label is a constant, the switch always compiles to a single O(1)
    // SwitchDispatch against a hash table built now — independent of the case count.
    void visit(const ast::SwitchStmt& s) override {
        // Fold + validate every label BEFORE any codegen: non-constant / non-scalar / duplicate all
        // throw here (a compile-time error at the offending label's span).
        std::vector<std::vector<std::string>> keys(s.cases.size());
        fum::unordered_set<std::string> seen;
        for (std::size_t ci = 0; ci < s.cases.size(); ++ci)
            for (const auto& valExpr : s.cases[ci].values) {
                std::optional<std::string> key;
                try {
                    key = foldConstKey(*valExpr);
                } catch (const KiritoError& e) {   // a constant that errors while folding (`case 1/0`)
                    throw KiritoError(e.what(), valExpr->span);
                }
                if (!key)
                    throw KiritoError("a switch case label must be a constant scalar — a literal or an "
                                      "expression over literals (a variable or a call is not allowed)",
                                      valExpr->span);
                if (!seen.insert(*key).second)
                    throw KiritoError("duplicate switch case value", valExpr->span);
                keys[ci].push_back(std::move(*key));
            }
        compileExpr(*s.subject);
        std::size_t tblIdx = proto_.switches.size();
        proto_.switches.emplace_back();                // reserve the slot; the index is stable across nesting
        emit(Op::SwitchDispatch, static_cast<uint32_t>(tblIdx), s.span);  // pops subject, jumps to an arm
        SwitchTable table;
        std::vector<std::size_t> endJumps;
        for (std::size_t ci = 0; ci < s.cases.size(); ++ci) {
            uint32_t arm = here();
            for (auto& k : keys[ci]) table.targets[std::move(k)] = arm;
            compileBlock(s.cases[ci].body);
            endJumps.push_back(emit(Op::Jump));
        }
        table.defaultTarget = here();                  // a missed key runs the default arm (or falls to end)
        if (s.hasDefault) compileBlock(s.defaultBody);
        for (std::size_t j : endJumps) patch(j, here());
        proto_.switches[tblIdx] = std::move(table);
        emit(Op::ClearResult);
    }

    // `class Name [(Base)]:` — eagerly compile the class body (caching it, surfacing any error in it
    // at compile time); push the base; BuildClass runs the body in a child scope and builds the class.
    void visit(const ast::ClassStmt& s) override {
        protoForBody(vm_, s.body, /*isFunction=*/true);  // compile + cache the body (errors propagate)
        if (s.base) compileExpr(*s.base);
        proto_.classes.push_back(&s);
        emit(Op::BuildClass, static_cast<uint32_t>(proto_.classes.size() - 1), s.span);  // pushes the class
        emitStore(s.name, s.span);   // bind the class name in its slot (frame or env), like `var Name = ...`
        emit(Op::ClearResult);
    }

    // `try: ... catch [T as e]: ... finally: ...`. Exception unwinding uses a runtime block stack
    // (SetupBlock/PopBlock + the executor's outer catch); break/continue/return crossing this try run
    // the finally via the frame-cleanup machinery. The finally body is duplicated per exit path
    // (normal / matched-handler / no-match / handler-exception) — small and simple beats clever.
    void visit(const ast::TryStmt& s) override {
        bool hasFin = s.hasFinally;
        bool hasHand = !s.handlers.empty();
        // On the exception path a `finally` runs with the in-flight exception value sitting on the
        // operand stack (unwind pushes it there). If that finally body contains break/continue/return,
        // their operand-stack cleanup math assumes a clean level and would mislocate the exception —
        // corrupting the stack (a crash / infinite loop). So on the exception paths we park the
        // exception in a hidden local across the finally body and reload it just before Reraise, so the
        // finally always runs at the clean operand height. `$` can't appear in a user name.
        std::string excName = hasFin ? "$exc" + std::to_string(tryCounter_++) : std::string();
        if (hasFin) ensureHiddenSlot(excName);
        // The exception's SPAN needs parking across the finally body just as its value does: a nested
        // try/catch in that body unwinds too, overwriting the frame's "span currently being unwound",
        // and the Reraise below would then blame the inner (already handled) exception's line for the
        // outer one that is genuinely still in flight.
        uint32_t spanSlot = hasFin ? proto_.excSpanSlots++ : 0;
        auto emitFinally = [this, &s, hasFin] { if (hasFin) compileBlock(s.finallyBody); };
        // Run the finally on an EXCEPTION path: stash exc (top of stack) + its span -> finally ->
        // reload both.
        auto emitFinallyExc = [this, &s, hasFin, &excName, spanSlot] {
            if (!hasFin) return;
            emitStore(excName, s.span);
            emit(Op::SaveExcSpan, spanSlot, s.span);
            compileBlock(s.finallyBody);
            emit(Op::RestoreExcSpan, spanSlot, s.span);
            emitLoad(excName, s.span);
        };

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
            if (!sawCatchAll) {  // no handler matched: run finally (exc parked), re-throw
                if (hasFin) { emit(Op::PopBlock); emitFinallyExc(); }
                emit(Op::Reraise, 0, s.span);
            }
            uint32_t lhandled = here();
            for (std::size_t j : toHandled) patch(j, lhandled);
            if (hasFin) { emit(Op::PopBlock); emitFinally(); }  // a handler ran: run finally, continue
            endJumps.push_back(emit(Op::Jump));  // -> Lend
        }

        if (hasFin) {  // Lfin: an exception in a handler (or in a type expr) — finally (exc parked), re-throw
            patch(finSetup, here());
            emitFinallyExc();
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
        else if (std::holds_alternative<ast::EllipsisTag>(e.value))
            emit(Op::LoadConst, addConst(vm_.ellipsis()));   // `...` -> the Ellipsis singleton
        else
            emit(Op::LoadNone);
    }

    void visit(const ast::NameExpr& e) override {
        // Inside an inlined body a param name resolves to its arg slot, overriding the resolver's stale
        // (body-as-nested-scope) annotation — this is the hygiene boundary: the body sees ONLY its
        // params (here) and globals (below), never caller locals.
        if (inlineRebind_)
            for (const auto& b : *inlineRebind_)
                if (b.name == e.name) {
                    if (b.capture) emit(Op::LoadVar, addEnvVarRaw(b.capDepth, b.capIndex, b.name), e.span);
                    else emitLoad(b.hidden, e.span);  // a parameter: its once-evaluated argument binding
                    return;
                }
        if (e.builtinSlot >= 0) emit(Op::LoadGlobal, static_cast<uint32_t>(e.builtinSlot), e.span);
        else if (e.envIndex >= 0) emit(Op::LoadVar, addEnvVar(e), e.span);
        else emitLoad(e.name, e.span);
    }

    void visit(const ast::UnaryExpr& e) override {
        if (tryEmitFolded(e)) return;
        compileExpr(*e.operand);
        emit(Op::UnaryOp, static_cast<uint32_t>(e.op), e.span);
    }

    void visit(const ast::BinaryExpr& e) override {
        if (tryEmitFolded(e)) return;
        compileExpr(*e.lhs);
        compileExpr(*e.rhs);
        emit(Op::BinaryOp, static_cast<uint32_t>(e.op), e.span);
    }

    void visit(const ast::LogicalExpr& e) override {
        if (tryEmitFolded(e)) return;
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
        // Inline a capture-free single-return lambda called directly (literal) or by an immutable
        // const-fn name (v1): no call frame, identical result. Candidacy excludes named/starred args.
        {
            const ast::FunctionExpr* fn = nullptr;
            std::vector<const ast::NameExpr*> caps;
            if (const ast::Expr* body = inlineBodyIfCandidate(e, fn, caps)) { emitInlinedCall(e, *fn, *body, caps); return; }
        }
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
        CallSpec spec;
        for (const auto& arg : e.args) {
            if (arg.name.empty()) ++spec.positional;
            else spec.names.push_back(arg.name);
        }
        // Fused method call: `obj.method(args)` compiles to one CallMethod (receiver + args on the
        // stack) instead of GetAttr (which allocates a bound method) followed by Call. Semantically
        // identical — CallMethod's runtime handler falls back to the exact GetAttr+Call path for every
        // case it can't fast-path (see applyMethodCall).
        if (const auto* mem = dynamic_cast<const ast::MemberExpr*>(e.callee.get())) {
            compileExpr(*mem->object);            // push the receiver (callee slot)
            for (const auto& arg : e.args)
                if (arg.name.empty()) compileExpr(*arg.value);
            for (const auto& arg : e.args)
                if (!arg.name.empty()) compileExpr(*arg.value);
            MethodCallSpec mspec;
            mspec.nameIndex = addName(mem->name);
            mspec.call = std::move(spec);
            proto_.methodCalls.push_back(std::move(mspec));
            emit(Op::CallMethod, static_cast<uint32_t>(proto_.methodCalls.size() - 1), e.span);
            return;
        }
        compileExpr(*e.callee);
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
        // A slice LITERAL (null object) is a subscript key -> build a Slice value (MakeSlice); a real
        // single-axis `obj[a:b:c]` keeps the existing GetSlice dispatch.
        if (e.object) compileExpr(*e.object);
        if (e.start) compileExpr(*e.start); else emit(Op::LoadNone);
        if (e.stop) compileExpr(*e.stop); else emit(Op::LoadNone);
        if (e.step) compileExpr(*e.step); else emit(Op::LoadNone);
        emit(e.object ? Op::GetSlice : Op::MakeSlice, 0, e.span);
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
                emit(Op::FormatValue, addFormatSpec(part.spec), e.span);
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
    int tryCounter_ = 0;   // unique hidden-local index per `try` (parks the in-flight exception)
    int retCounter_ = 0;   // unique hidden-local index per value-parking `return` crossing a cleanup
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
    // Fast path: a function's Proto is cached on its AST node, so a call skips the protoCache_ hashmap
    // probe (A3). The cache still owns the Proto; this pointer is only ever set to a cache-owned Proto.
    if (fnDef && fnDef->compiledProto) return fnDef->compiledProto;
    const Proto* p = protoForImpl(vm, &body, [&](Compiler& c) { c.compile(body, isFunction, fnDef); });
    if (fnDef) fnDef->compiledProto = p;
    return p;
}

// Compile a single expression (e.g. a parameter default) to its own Proto, cached by the expr's
// address. The Proto evaluates the expression and returns its value.
inline const Proto* protoForExpr(KiritoVM& vm, const ast::Expr& e) {
    return protoForImpl(vm, &e, [&](Compiler& c) { c.compileSingleExpr(e); });
}

}  // namespace kirito

#endif
