#ifndef KIRITO_BYTECODE_VM_HPP
#define KIRITO_BYTECODE_VM_HPP

#include <cassert>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "bytecode.hpp"
#include "class_value.hpp"
#include "collections.hpp"
#include "compiler.hpp"
#include "environment.hpp"
#include "function.hpp"
#include "object.hpp"
#include "vm.hpp"

// The bytecode execution engine (Kirito's sole engine). Included by the umbrella AFTER runtime.hpp,
// because it dispatches through the shared operation helpers (applyCall / applyBinaryOp / applyUnaryOp
// / evalMemberGet / checkPrivateAccess) and value methods (iterate / setItem / ...) that runtime.hpp
// defines. It owns no semantics of its own — only the stack-machine control structure.

namespace kirito {

// An internal iteration cursor: either the eagerly-materialised items of a for-loop's iterable plus a
// position, OR a lazy pull-based source (a stream) that produces one item per step. It lives only on a
// frame's operand stack (so the GC traces its items via children()) between GetIter and the loop's
// end; it is never visible to Kirito code. kind() is a sentinel.
class IterCursor : public Object {
public:
    std::vector<Handle> items;
    std::size_t idx = 0;
    std::unique_ptr<LazyIterator> lazy;   // set for a stream: pull one item per step (items unused)
    Handle source{};                      // the lazily-consumed iterable — kept rooted so its buffer/
                                          // stream survives the whole loop (the LazyIterator re-derefs it)
    ValueKind kind() const override { return ValueKind::None; }  // internal; never exposed to user code
    std::string typeName() const override { return "iterator"; }
    bool truthy() const override { return true; }
    std::string str(StringifyCtx&) const override { return "<iterator>"; }
    bool equals(const ObjectArena&, const Object& o) const override { return this == &o; }
    void children(std::vector<Handle>& out) const override {
        out.insert(out.end(), items.begin(), items.end());
        if (lazy) out.push_back(source);
    }
};

// Executes one Proto frame against a scope. The operand stack is a single C++ vector whose first
// three slots hold the frame's scope / last-expression result / owning class (for private + _super_),
// with the working operand stack above them; the whole vector is registered as a GC root region so
// every live value is traced. One frame == one BytecodeVM instance, created and destroyed on the
// stack; a nested call spins up its own. Non-movable so the registered &stack_ never dangles.
class BytecodeVM {
public:
    BytecodeVM(KiritoVM& vm, Handle scope, Handle ownerClass, bool hasOwner,
               std::string frameLabel = "<module>")
        : vm_(vm), hasOwner_(hasOwner), frameLabel_(std::move(frameLabel)) {
        stack_.reserve(16);
        stack_.push_back(scope);                              // kScope
        stack_.push_back(vm.none());                          // kResult
        stack_.push_back(hasOwner ? ownerClass : vm.none());  // kOwner
        vm_.pushAuxRoots(&stack_);
    }
    ~BytecodeVM() { vm_.popAuxRoots(); }
    BytecodeVM(const BytecodeVM&) = delete;
    BytecodeVM& operator=(const BytecodeVM&) = delete;

    Handle run(const Proto& proto, std::span<const Handle> paramValues = {}) {
        for (uint32_t i = 0; i < proto.localCount; ++i) push(vm_.undefined());  // reserve frame slots
        // Place each non-captured parameter's argument value straight into its frame slot, so a param
        // read is a direct slot access (no scope-chain name walk / unwritten-slot fallback). Captured
        // params (paramSlots[i] < 0) are left to the scope env, where the closure can reach them.
        for (std::size_t i = 0; i < proto.paramSlots.size() && i < paramValues.size(); ++i)
            if (proto.paramSlots[i] >= 0) setLocal(static_cast<uint32_t>(proto.paramSlots[i]), paramValues[i]);
        // Pre-declare this function's captured-local env slots, immediately after the parameters that
        // callFull already defined (so their fixed indices are P, P+1, ...). A closure reaches them by
        // LoadVar(depth, index); the local's own `var` overwrites the placeholder in place. Empty for
        // module/class bodies (a module's slots are pre-declared by the resolver into the live scope).
        if (!proto.envSlots.empty()) {
            EnvValue& env = static_cast<EnvValue&>(vm_.arena().deref(scope()));
            for (const auto& name : proto.envSlots) env.define(name, vm_.undefined());
        }
        const std::vector<Instr>& code = proto.code;
        std::size_t ip = 0;
        while (true) {
          try {
            while (ip < code.size()) {
            const Instr& in = code[ip++];
            switch (in.op) {
                case Op::LoadConst: { push(proto.consts[in.a]); } break;
                case Op::LoadNone: { push(vm_.none()); } break;
                case Op::LoadResult: { push(result()); } break;
                case Op::SetResult: { setResult(pop()); } break;
                case Op::ClearResult: { setResult(vm_.none()); } break;
                case Op::Pop: { pop(); } break;
                case Op::Dup: { push(peek(0)); } break;

                case Op::LoadName: {
                    auto found = envLookup(vm_.arena(), scope(), proto.names[in.a]);
                    if (!found) throw KiritoError("name '" + proto.names[in.a] + "' is not defined", in.span);
                    push(*found);
                } break;
                case Op::LoadGlobal: { push(vm_.globalSlot(in.a)); } break;  // O(1): no scope-chain walk
                case Op::LoadVar: {  // O(1): walk `depth` parents, read slot `index` — no name lookup
                    const EnvVarRef& ref = proto.envVars[in.a];
                    EnvValue& e = envAtDepth(ref.depth);
                    // The slot is exact by construction, EXCEPT when a closure is placed on a scope
                    // chain shorter/emptier than it was compiled for — a spawned nested function whose
                    // enclosing locals do NOT cross to the worker gets empty stand-in scopes, so the
                    // enclosing slot is out of range. That is a clean "not defined", not a wrong read.
                    if (ref.index >= e.size())
                        throw KiritoError("name '" + ref.name + "' is not defined", in.span);
                    assert(e.nameAt(ref.index) == ref.name);  // otherwise the (depth,index) is exact
                    Handle h = e.at(ref.index);
                    if (h == vm_.undefined())  // declared but not yet assigned (its `var` hasn't run): strict
                        throw KiritoError("name '" + ref.name + "' is not defined", in.span);
                    push(h);
                } break;
                case Op::AssignVar: {  // O(1) rebind of the (depth, index) env slot — no name lookup
                    Handle v = pop();
                    const EnvVarRef& ref = proto.envVars[in.a];
                    EnvValue& e = envAtDepth(ref.depth);
                    if (ref.index >= e.size())   // an enclosing slot that did not cross (see LoadVar)
                        throw KiritoError("name '" + ref.name + "' is not defined", in.span);
                    assert(e.nameAt(ref.index) == ref.name);
                    if (e.at(ref.index) == vm_.undefined())  // rebinding before the var executed
                        throw KiritoError("name '" + ref.name + "' is not defined", in.span);
                    e.setAt(ref.index, v);
                } break;
                case Op::StoreName: {
                    Handle v = pop();
                    static_cast<EnvValue&>(vm_.arena().deref(scope())).define(proto.names[in.a], v);
                } break;
                case Op::AssignName: {
                    Handle v = pop();
                    if (!envAssign(vm_.arena(), scope(), proto.names[in.a], v))
                        throw KiritoError("name '" + proto.names[in.a] + "' is not defined", in.span);
                } break;

                case Op::LoadLocal: {  // O(1): direct frame slot, no name lookup
                    Handle h = getLocal(in.a);
                    if (h == vm_.undefined())  // read before assignment: strict, no name walk
                        throw KiritoError("name '" + proto.localNames[in.a] + "' is not defined", in.span);
                    push(h);
                } break;
                case Op::StoreLocal: { setLocal(in.a, pop()); } break;
                case Op::AssignLocal: {  // O(1): direct frame slot, no name lookup
                    Handle v = pop();
                    if (getLocal(in.a) == vm_.undefined())  // rebinding before the var executed: strict
                        throw KiritoError("name '" + proto.localNames[in.a] + "' is not defined", in.span);
                    setLocal(in.a, v);
                } break;

                case Op::UnaryOp: {
                    Handle operand = peek(0);
                    Handle r = located(in.span, [&] { return applyUnaryOp(vm_, static_cast<UnOp>(in.a), operand); });
                    pop();
                    push(r);
                } break;
                case Op::BinaryOp: {
                    Handle lhs = peek(1), rhs = peek(0);  // operands stay rooted on the stack while we operate
                    Handle r = located(in.span, [&] { return applyBinaryOp(vm_, static_cast<BinOp>(in.a), lhs, rhs); });
                    pop(); pop();
                    push(r);
                } break;

                case Op::Jump: { ip = in.a; } break;
                // truthy() may throw (a user _bool_ returning a non-Bool); wrap in located() so the
                // if/while condition error carries its real line:col instead of 0:0 (A05-4).
                case Op::PopJumpIfFalse: { Handle v = pop(); if (!located(in.span, [&]{ return vm_.arena().deref(v).truthy(); })) ip = in.a; } break;
                case Op::PopJumpIfTrue: { Handle v = pop(); if (located(in.span, [&]{ return vm_.arena().deref(v).truthy(); })) ip = in.a; } break;
                case Op::JumpIfFalseOrPop: {
                    if (!located(in.span, [&]{ return vm_.arena().deref(peek(0)).truthy(); })) ip = in.a; else pop();
                } break;
                case Op::JumpIfTrueOrPop: {
                    if (located(in.span, [&]{ return vm_.arena().deref(peek(0)).truthy(); })) ip = in.a; else pop();
                } break;

                case Op::Call: {
                    const CallSpec& spec = proto.calls[in.a];
                    std::size_t total = spec.positional + spec.names.size();
                    std::size_t base = stack_.size() - total;   // first argument slot
                    Handle callee = stack_[base - 1];            // sits just below the arguments
                    std::span<const Handle> pos(stack_.data() + base, spec.positional);
                    std::vector<NamedArg> named;
                    named.reserve(spec.names.size());
                    for (std::size_t i = 0; i < spec.names.size(); ++i)
                        named.push_back({spec.names[i], stack_[base + spec.positional + i]});
                    Handle r = located(in.span, [&] { return applyCall(vm_, callee, pos, named); });
                    stack_.resize(base - 1);                     // pop callee + all arguments
                    push(r);
                } break;
                case Op::MakeFunction: {
                    auto fn = std::make_unique<KiFunction>(proto.funcs[in.a], scope());
                    fn->sourceFile = vm_.currentChunkFile();
                    push(vm_.alloc(std::move(fn)));
                } break;
                case Op::BuildClass: {
                    const ast::ClassStmt& cs = *proto.classes[in.a];
                    RootScope rs(vm_);
                    Handle base{};
                    bool hasBase = cs.base != nullptr;
                    if (hasBase) {
                        base = rs.add(pop());                  // base pushed just before BuildClass
                        Object& baseObj = vm_.arena().deref(base);
                        if (baseObj.kind() != ValueKind::Class)  // a non-class base would be a bad downcast (UB) later
                            throw KiritoError("base class must be a class, got " + baseObj.typeName(), cs.span);
                    }
                    Handle classScope = rs.add(vm_.newScope(scope()));  // run the class body in a child scope
                    const Proto* body = protoForBody(vm_, cs.body, /*isFunction=*/true);  // compiled+cached already
                    { BytecodeVM sub(vm_, classScope, vm_.none(), false); sub.run(*body); }
                    auto cls = std::make_unique<ClassValue>();
                    cls->name = cs.name;
                    cls->base = base;
                    cls->hasBase = hasBase;
                    for (const auto& [k, v] : static_cast<EnvValue&>(vm_.arena().deref(classScope)).locals())
                        if (v != vm_.undefined())                       // skip a pre-declared slot never assigned
                            cls->methods[k] = v;                        // the names the body defined are the methods
                    Handle clsHandle = rs.add(vm_.alloc(std::move(cls)));
                    auto& klass = static_cast<ClassValue&>(vm_.arena().deref(clsHandle));
                    klass.selfHandle = clsHandle;
                    // Own the methods so their bodies may touch privates / resolve `_super_` against
                    // THIS class. Ownership must attach to the (class, method) BINDING, never be stamped
                    // on the shared function OBJECT: a function reachable elsewhere — a module-level fn
                    // adopted as a method, or one function assigned as a method of two classes — would
                    // otherwise have its single `ownerClass` field overwritten by the LAST class to
                    // reference it (corrupting `_super_`, and both falsely-denying AND letting an
                    // external call bypass privacy). So install an OWNED CLONE and leave the original
                    // untouched. Rebind the class-scope local too, so a method that calls a sibling as a
                    // free variable (through the captured class scope) also reaches the owned clone.
                    {
                        auto& classEnv = static_cast<EnvValue&>(vm_.arena().deref(classScope));
                        std::vector<std::pair<std::string, Handle>> owned;
                        for (const auto& [mname, mh] : klass.methods) {
                            Object& mo = vm_.arena().deref(mh);
                            if (mo.kind() != ValueKind::Function) continue;
                            auto& fn = static_cast<KiFunction&>(mo);
                            auto clone = std::make_unique<KiFunction>(&fn.def(), fn.closure());
                            clone->sourceFile = fn.sourceFile;
                            clone->ownerClass = clsHandle;
                            clone->hasOwner = true;
                            owned.emplace_back(mname, rs.add(vm_.alloc(std::move(clone))));
                        }
                        for (auto& [mname, ch] : owned) { klass.methods[mname] = ch; classEnv.define(mname, ch); }
                    }
                    vm_.registerClass(cs.name, clsHandle);  // so serialize/dump can reconstruct instances
                    // Leave the class on the operand stack; the compiler binds cs.name right after via
                    // StoreLocal/StoreName (exactly like `var Name = ...`), so a non-captured class name
                    // lands in its frame slot rather than only the scope env — consistent with the slot
                    // model now that there is no unwritten-slot name-walk fallback to paper over it.
                    push(clsHandle);
                } break;

                case Op::GetAttr: {
                    Handle obj = peek(0);
                    Handle r = located(in.span, [&] {
                        return evalMemberGet(vm_, obj, proto.names[in.a], ownerClass(), hasOwner_, in.span);
                    });
                    pop();
                    push(r);
                } break;
                case Op::SetAttr: {
                    Handle obj = peek(0), value = peek(1);   // stack: [value, obj]
                    const std::string& name = proto.names[in.a];
                    located(in.span, [&] {
                        checkPrivateAccess(vm_, obj, name, ownerClass(), hasOwner_, in.span);
                        vm_.arena().deref(obj).setAttr(vm_, name, value);
                        return vm_.none();
                    });
                    pop(); pop();
                } break;
                case Op::GetItem: {
                    std::size_t n = in.a, base = stack_.size() - n;
                    Handle obj = stack_[base - 1];
                    std::vector<Handle> keys(stack_.begin() + static_cast<std::ptrdiff_t>(base), stack_.end());
                    Handle r = located(in.span, [&] { return vm_.arena().deref(obj).getItem(vm_, keys); });
                    stack_.resize(base - 1);
                    push(r);
                } break;
                case Op::SetItem: {
                    std::size_t n = in.a, base = stack_.size() - n;   // stack: [value, obj, keys...]
                    Handle obj = stack_[base - 1], value = stack_[base - 2];
                    std::vector<Handle> keys(stack_.begin() + static_cast<std::ptrdiff_t>(base), stack_.end());
                    located(in.span, [&] { vm_.arena().deref(obj).setItem(vm_, keys, value); return vm_.none(); });
                    stack_.resize(base - 2);
                } break;
                case Op::GetSlice: {
                    Handle obj = peek(3), start = peek(2), stop = peek(1), step = peek(0);
                    Handle r = located(in.span, [&] { return vm_.arena().deref(obj).slice(vm_, start, stop, step); });
                    pop(); pop(); pop(); pop();
                    push(r);
                } break;

                case Op::BuildList:
                case Op::BuildPack: {  // bare-comma packing builds the same List as a list literal —
                                       // one implementation, two opcodes kept only for disassembly clarity
                    std::size_t n = in.a, base = stack_.size() - n;
                    auto list = std::make_unique<ListVal>();
                    list->elems.reserve(n);
                    for (std::size_t i = 0; i < n; ++i) list->elems.push_back(stack_[base + i]);
                    Handle r = vm_.alloc(std::move(list));
                    stack_.resize(base);
                    push(r);
                } break;
                case Op::BuildSet: {
                    std::size_t n = in.a, base = stack_.size() - n;
                    auto set = std::make_unique<SetVal>();
                    located(in.span, [&] {
                        for (std::size_t i = 0; i < n; ++i) set->add(vm_.arena(), stack_[base + i]);
                        return vm_.none();
                    });
                    Handle r = vm_.alloc(std::move(set));
                    stack_.resize(base);
                    push(r);
                } break;
                case Op::BuildDict: {
                    std::size_t pairs = in.a, base = stack_.size() - 2 * pairs;
                    auto dict = std::make_unique<DictVal>();
                    located(in.span, [&] {
                        for (std::size_t i = 0; i < pairs; ++i)
                            dict->set(vm_.arena(), stack_[base + 2 * i], stack_[base + 2 * i + 1]);
                        return vm_.none();
                    });
                    Handle r = vm_.alloc(std::move(dict));
                    stack_.resize(base);
                    push(r);
                } break;

                case Op::FormatValue: {
                    Handle v = peek(0);
                    const std::string& spec = proto.names[in.a];
                    std::string s = located(in.span, [&] {
                        return spec.empty() ? vm_.stringify(v) : applyFormatSpec(vm_, v, spec);
                    });
                    Handle r = vm_.makeString(std::move(s));
                    pop();
                    push(r);
                } break;
                case Op::BuildString: {
                    std::size_t n = in.a, base = stack_.size() - n;
                    std::string out;
                    for (std::size_t i = 0; i < n; ++i)
                        out += static_cast<const StrVal&>(vm_.arena().deref(stack_[base + i])).value();
                    Handle r = vm_.makeString(std::move(out));
                    stack_.resize(base);
                    push(r);
                } break;

                case Op::GetIter: {
                    Handle iterable = peek(0);
                    // Prefer a lazy (stream) cursor: pull one element per step so a large file / stdin is
                    // not buffered to EOF (A10-5). Fall back to eager iterate() for everything else.
                    auto lazy = located(in.span, [&] { return vm_.arena().deref(iterable).lazyIterate(vm_, iterable); });
                    auto cursor = std::make_unique<IterCursor>();
                    // Root freshly-materialised items across the alloc(cursor) below — alloc() runs a GC
                    // *before* it inserts the cursor, so the not-yet-rooted cursor's items would be swept
                    // (A05-1). The scope must outlive alloc, so it is hoisted here (not scoped to `else`);
                    // items produced by iterate() (fresh 1-char Strings, per-byte Integers, user _iter_
                    // results) are not the iterable's children(), so `iterable` on the stack can't protect them.
                    RootScope rs(vm_);
                    if (lazy) {
                        cursor->lazy = std::move(lazy);
                        cursor->source = iterable;   // keep the stream rooted for the loop's duration
                    } else {
                        auto items = located(in.span, [&] { return vm_.arena().deref(iterable).iterate(vm_); });
                        if (!items)
                            throw KiritoError("type '" + vm_.arena().deref(iterable).typeName() + "' is not iterable", in.span);
                        for (Handle it : items.value()) rs.add(it);
                        cursor->items = std::move(items.value());
                    }
                    Handle r = vm_.alloc(std::move(cursor));  // rs still alive here → items survive the GC
                    pop();
                    push(r);
                } break;
                case Op::ForIter: {
                    // peek(0) stays valid across a lazy next() (the cursor is on the operand stack, a GC
                    // root); the produced item is pushed immediately, so no unrooted gap.
                    if (static_cast<IterCursor&>(vm_.arena().deref(peek(0))).lazy) {
                        auto step = located(in.span, [&] {
                            return static_cast<IterCursor&>(vm_.arena().deref(peek(0))).lazy->next(vm_);
                        });
                        if (!step) { pop(); ip = in.a; }  // exhausted: drop cursor, exit loop
                        else push(*step);
                    } else {
                        auto& cur = static_cast<IterCursor&>(vm_.arena().deref(peek(0)));
                        if (cur.idx >= cur.items.size()) { pop(); ip = in.a; }  // exhausted: drop cursor, exit loop
                        else push(cur.items[cur.idx++]);
                    }
                } break;
                case Op::Unpack: {
                    const UnpackSpec& spec = proto.unpacks[in.a];
                    Handle iterable = peek(0);   // stays rooted while spreadValues iterates/allocates
                    std::vector<Handle> slots = located(in.span, [&] {
                        return spreadValues(vm_, iterable, spec.count, spec.starIndex, in.span);
                    });
                    pop();  // drop the iterable
                    for (std::size_t i = spec.count; i-- > 0;) push(slots[i]);  // reversed: slot 0 ends on top
                } break;
                case Op::SwitchMatch: {
                    Handle v = peek(0), subj = peek(1);
                    auto vk = scalarSwitchKey(vm_, v);
                    if (!vk) throw KiritoError("switch case value must be Integer, Float, String, Bool, or None", in.span);
                    auto sk = scalarSwitchKey(vm_, subj);
                    bool match = sk.has_value() && *sk == *vk;
                    pop(); pop();
                    push(vm_.makeBool(match));
                } break;
                case Op::SwitchDispatch: {
                    // O(1): hash the subject's key ONCE and jump to the precompiled arm offset (or the
                    // default). A non-scalar / NaN subject has no key -> default, matching SwitchMatch.
                    Handle subj = pop();
                    const SwitchTable& tbl = proto.switches[in.a];
                    auto sk = scalarSwitchKey(vm_, subj);
                    ip = tbl.defaultTarget;
                    if (sk) {
                        auto it = tbl.targets.find(*sk);
                        if (it != tbl.targets.end()) ip = it->second;
                    }
                } break;

                case Op::SetupBlock: { blocks_.push_back({in.a, stack_.size()}); } break;
                case Op::PopBlock: { blocks_.pop_back(); } break;
                case Op::Reraise: { Handle v = pop(); throw KiritoThrow{v, excSpan_}; } break;  // keep the original site
                case Op::ExcMatch: {
                    Handle type = pop(), exc = pop();
                    push(vm_.makeBool(isInstanceOf(vm_, exc, type)));
                } break;
                case Op::Throw: { Handle v = pop(); throw KiritoThrow{v, in.span}; } break;
                case Op::Return: { return pop(); } break;
                default: {
                    // An opcode the dispatch doesn't know means a corrupt/truncated Proto — a VM
                    // invariant violation, not a program error. Fail loudly instead of misbehaving.
                    throw KiritoError("internal error: unknown bytecode opcode", in.span);
                } break;
            }
            }
            return vm_.none();  // a Proto always ends in Return; reaching here means ip ran off the end
          } catch (KiritoError& e) {
            // Internal/runtime errors are surfaced to Kirito `catch` as a String exception value.
            // Catch KiritoError BEFORE KiritoThrow — KiritoError derives from KiritoThrow, so
            // swapping the order would send a KiritoError into the throw-handler with
            // .value == Handle{} and mis-unwind.
            appendFrame(e.traceback, ip, code);
            Handle s = vm_.makeString(e.what());
            if (!unwind(s, e.span, ip)) throw;
            vm_.setLastTraceback(e.traceback);
          } catch (KiritoThrow& t) {  // non-const: append this frame to the traceback before re-throwing
            appendFrame(t.traceback, ip, code);
            if (!unwind(t.value, t.span, ip)) throw;
            vm_.setLastTraceback(t.traceback);  // handled here -> expose the chain to sys.traceback()
          } catch (const std::exception& e) {
            // Any other native exception is also catchable (as a String), guarding the whole boundary.
            // Mirror the two arms above: record THIS frame in a fresh traceback and, when handled here,
            // expose it via sys.traceback() — otherwise the handler would see the PREVIOUS error's stale
            // chain and this failing frame would be missing (A04-2).
            std::vector<TraceFrame> tb;
            appendFrame(tb, ip, code);
            Handle s = vm_.makeString(e.what());
            if (!unwind(s, SourceSpan{}, ip)) throw;
            vm_.setLastTraceback(tb);
          }
        }
    }

private:
    static constexpr std::size_t kScope = 0, kResult = 1, kOwner = 2, kOperandBase = 3;
    Handle scope() const { return stack_[kScope]; }
    Handle ownerClass() const { return stack_[kOwner]; }
    Handle result() const { return stack_[kResult]; }
    void setResult(Handle h) { stack_[kResult] = h; }
    void push(Handle h) { stack_.push_back(h); }
    Handle pop() { Handle h = stack_.back(); stack_.pop_back(); return h; }
    Handle peek(std::size_t fromTop) const { return stack_[stack_.size() - 1 - fromTop]; }
    // Slot-addressed locals live just above the reserved scope/result/owner slots (the operand stack
    // sits above them); reserved once at frame entry and never popped, so these indices stay valid.
    Handle getLocal(uint32_t i) const { return stack_[kOperandBase + i]; }
    void setLocal(uint32_t i, Handle h) { stack_[kOperandBase + i] = h; }
    // The EnvValue `depth` hops up the scope chain from this frame — the target of LoadVar/AssignVar.
    EnvValue& envAtDepth(uint16_t depth) {
        Handle s = scope();
        for (uint16_t d = 0; d < depth; ++d) s = static_cast<EnvValue&>(vm_.arena().deref(s)).parent();
        return static_cast<EnvValue&>(vm_.arena().deref(s));
    }

    template <typename F>
    auto located(SourceSpan span, F&& fn) -> decltype(fn()) {
        try {
            return fn();
        } catch (KiritoError& err) {
            if (err.span.line == 0) err.span = span;
            throw;
        }
    }

    // Route an in-flight exception to the innermost active try/with block: pop it, unwind the operand
    // stack to that block's height, hand it the exception value, and jump to its handler. Returns
    // false (leaving state untouched) when no block is active — the exception escapes this frame.
    // The exception's original span is remembered so a later Reraise reports its true site.
    bool unwind(Handle exc, SourceSpan span, std::size_t& ip) {
        if (blocks_.empty()) return false;
        excSpan_ = span;
        Block b = blocks_.back();
        blocks_.pop_back();
        stack_.resize(b.stackHeight);
        push(exc);
        ip = b.target;
        return true;
    }

    // Record THIS frame on an unwinding error's traceback: its label + file + the line it was executing
    // (the faulting instruction for the innermost frame, the call site for the outer ones). Called once
    // per frame the exception passes through, building the chain innermost-first.
    void appendFrame(std::vector<TraceFrame>& tb, std::size_t ip, const std::vector<Instr>& code) {
        uint32_t ln = (ip >= 1 && ip <= code.size()) ? code[ip - 1].span.line : 0;
        tb.push_back(TraceFrame{frameLabel_, vm_.currentChunkFile(), ln});
    }

    struct Block {
        uint32_t target;        // ip of the handler/finally landing pad
        std::size_t stackHeight;  // operand-stack size at SetupBlock (restored on unwind)
    };

    KiritoVM& vm_;
    std::vector<Handle> stack_;
    std::vector<Block> blocks_;
    SourceSpan excSpan_{};  // span of the exception currently being unwound (for Reraise)
    bool hasOwner_;
    std::string frameLabel_;  // function name / "<function>" / "<module>" — for traceback frames
};

inline Handle runBytecodeBody(KiritoVM& vm, Handle scope, const ast::Block& body, Handle ownerClass,
                              bool hasOwner, bool isFunction, std::string frameLabel,
                              const ast::FunctionExpr* fnDef, std::span<const Handle> paramValues) {
    // Root the scope (and owning class) across BOTH compilation and execution: first-run compilation
    // materialises constants and so may trigger GC, and the top-level scope is not otherwise rooted
    // here (unlike a function call scope, which callFull already roots). The BytecodeVM re-roots them
    // too once constructed; this just covers the compile window before it exists.
    RootScope rs(vm);
    rs.add(scope);
    if (hasOwner) rs.add(ownerClass);
    const Proto* proto = protoForBody(vm, body, isFunction, fnDef);
    BytecodeVM bc(vm, scope, ownerClass, hasOwner, std::move(frameLabel));
    return bc.run(*proto, paramValues);
}

inline Handle runBytecodeExpr(KiritoVM& vm, Handle scope, const ast::Expr& e) {
    RootScope rs(vm);
    rs.add(scope);
    const Proto* proto = protoForExpr(vm, e);
    BytecodeVM bc(vm, scope, vm.none(), false);
    return bc.run(*proto);
}

}  // namespace kirito

#endif
