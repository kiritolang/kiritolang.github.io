#ifndef KIRITO_NATIVE_HPP
#define KIRITO_NATIVE_HPP

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "builtins.hpp"
#include "function.hpp"
#include "module.hpp"
#include "object.hpp"
#include "value.hpp"
#include "vm.hpp"

namespace kirito {

// Shared argument-extraction helpers for native functions/modules — one place to dereference a
// Handle and type-check it, so modules don't each reimplement asStr/asInt (DRY). `who` names the
// caller for the error message.
inline const std::string& argString(KiritoVM& vm, Handle h, const char* who) {
    const Object& o = vm.arena().deref(h);
    if (o.kind() != ValueKind::String) throw KiritoError(std::string(who) + " expects a String");
    return static_cast<const StrVal&>(o).value();
}
inline int64_t argInt(KiritoVM& vm, Handle h, const char* who) {
    const Object& o = vm.arena().deref(h);
    if (o.kind() != ValueKind::Integer) throw KiritoError(std::string(who) + " expects an Integer");
    return static_cast<const IntVal&>(o).value();
}
// A Kirito String is byte-transparent, so it can hold an embedded NUL — but the OS filesystem APIs
// (fopen / std::filesystem) are NUL-terminated and SILENTLY TRUNCATE the path at the first NUL,
// which defeats any suffix/extension check ("safe.txt\0.sh" opens "safe.txt"): a validation/sandbox
// bypass. Reject it up front (Python raises the same ValueError). One place for every path entry.
inline void requireNoNulPath(const std::string& p, const char* who) {
    if (p.find('\0') != std::string::npos)
        throw KiritoError(std::string(who) + ": embedded NUL byte in path");
}
// Reject an under-arity positional call before the impl dereferences a[0]/a[1]/... `makeMethod`'s
// positional fast path forwards the call's args verbatim (no padding), so a method whose body
// indexes a fixed `a[i]` must guard first — else it reads past the span (UB). One place to do it so
// every native method gives the same clean "expected at least N argument(s)" error.
inline void requireArgs(std::span<const Handle> a, std::size_t n, const char* who) {
    if (a.size() < n)
        throw KiritoError(std::string(who) + "() expected at least " + std::to_string(n) +
                          " argument(s), got " + std::to_string(a.size()));
}

// Resolve a slice — start/stop/step given as Integer-or-None handles — to the concrete indices over
// [0, len): negative indices count from the end, out-of-range bounds clamp, a negative step iterates
// downward, and a zero step throws. Shared by String/Bytes/List/Array slicing so the index math lives
// in exactly one place.
inline std::vector<int64_t> sliceIndices(KiritoVM& vm, int64_t len, Handle sH, Handle eH, Handle stH) {
    auto opt = [&](Handle h) -> std::optional<int64_t> {
        const Object& o = vm.arena().deref(h);
        if (o.kind() == ValueKind::None) return std::nullopt;
        if (o.kind() != ValueKind::Integer) throw KiritoError("slice indices must be Integer or None");
        return static_cast<const IntVal&>(o).value();
    };
    std::optional<int64_t> so = opt(sH), eo = opt(eH), sto = opt(stH);
    int64_t step = sto.value_or(1);
    if (step == 0) throw KiritoError("slice step cannot be zero");
    int64_t lower = step < 0 ? -1 : 0, upper = step < 0 ? len - 1 : len, start, stop;
    if (!so) start = step < 0 ? upper : lower;
    else { start = *so; if (start < 0) { start += len; if (start < lower) start = lower; }
           else if (start > upper) start = upper; }
    if (!eo) stop = step < 0 ? lower : upper;
    else { stop = *eo; if (stop < 0) { stop += len; if (stop < lower) stop = lower; }
           else if (stop > upper) stop = upper; }
    // Count-driven, not `i += step`: a near-INT64_MAX step would signed-overflow that increment (UB)
    // and fail to terminate, yielding out-of-range indices (OOB read / segfault). start/stop are
    // clamped to [-1, len], so the span is small and the element count can't overflow.
    std::vector<int64_t> idx;
    if (step > 0 && stop > start) {
        int64_t count = (stop - start - 1) / step + 1;            // ceil((stop-start)/step), no overflow
        idx.reserve(static_cast<std::size_t>(count));
        for (int64_t k = 0; k < count; ++k) idx.push_back(start + k * step);
    } else if (step < 0 && start > stop) {
        uint64_t span = static_cast<uint64_t>(start - stop);
        uint64_t mag = static_cast<uint64_t>(-(step + 1)) + 1ULL;  // |step|, safe even at INT64_MIN
        int64_t count = static_cast<int64_t>((span - 1) / mag + 1);
        idx.reserve(static_cast<std::size_t>(count));
        for (int64_t k = 0; k < count; ++k) idx.push_back(start + k * step);
    }
    return idx;
}

// ============================================================================================
// Extension API — how C++ code adds new functions, modules, and types to Kirito.
//
//  * A one-off function:   vm.registerGlobal("now", myFn);
//  * A whole module:       struct MyMod : NativeModule { ... };  vm.install<MyMod>();
//  * A new object type:    struct MyType : NativeClass<MyType> { ... };  expose a constructor
//                          with vm.registerGlobal("MyType", factoryFn);
//
// Everything flows through the same uniform Object protocol, so an extension is indistinguishable
// from a built-in to the evaluator.
// ============================================================================================

// Fluent helper passed to NativeModule::setup() to register a module's members with no boilerplate.
class ModuleBuilder {
public:
    ModuleBuilder(KiritoVM& vm, Handle module, ModuleValue& mod) : vm_(vm), module_(module), mod_(mod) {}

    // The module's own value handle, so members can capture it to reach *sibling* members at call
    // time (e.g. io.print resolving the current io.stdout, which the user may have reassigned).
    Handle moduleHandle() const { return module_; }

    ModuleBuilder& fn(std::string name, NativeFn impl) {
        mod_.setMember(name, vm_.alloc(std::make_unique<NativeFunction>(name, std::move(impl))));
        return *this;
    }
    // With a declared signature: the function then accepts keyword arguments and defaults, and
    // `inspect` shows its parameters/types and return type. Params: {"x"} / {"x","Int"} /
    // {"x","Int", vm().makeInt(0)} (with default).
    ModuleBuilder& fn(std::string name, std::vector<NativeParam> sig, std::string returnType, NativeFn impl) {
        // Root any heap-allocated parameter defaults across the NativeFunction alloc: the pending
        // function isn't arena-reachable yet, so a GC during alloc would sweep an unrooted default
        // (e.g. hash.hmac's "sha256"), leaving the function holding a dangling default handle.
        RootScope rs(vm_);
        for (const auto& p : sig) if (p.hasDefault) rs.add(p.defaultValue);
        mod_.setMember(name, vm_.alloc(std::make_unique<NativeFunction>(
            name, std::move(sig), std::move(returnType), std::move(impl))));
        return *this;
    }
    // A variadic native that also accepts keyword arguments (impl receives positional args + named).
    ModuleBuilder& kwfn(std::string name, NativeFnKw impl) {
        mod_.setMember(name, vm_.alloc(std::make_unique<NativeFunction>(name, std::move(impl))));
        return *this;
    }
    ModuleBuilder& value(const std::string& name, Handle h) {
        mod_.setMember(name, h);
        return *this;
    }
    // Bind `name` to the same member as an already-registered `existing` (a second public name).
    ModuleBuilder& alias(const std::string& name, const std::string& existing) {
        auto it = mod_.members.find(existing);
        if (it == mod_.members.end()) throw KiritoError("alias target '" + existing + "' not registered");
        mod_.setMember(name, it->second);
        return *this;
    }
    KiritoVM& vm() { return vm_; }

private:
    KiritoVM& vm_;
    Handle module_;
    ModuleValue& mod_;
};

// Inherit and implement to define a module in C++: give it a name and register its members.
class NativeModule {
public:
    virtual ~NativeModule() = default;
    virtual std::string name() const = 0;
    virtual void setup(ModuleBuilder&) = 0;
};

// CRTP convenience base for a C++-authored object type. Fills in sane protocol defaults (identity
// equality, a name) so a subclass overrides only the slots it actually supports (binary, getAttr,
// call, ...). Derived must define `static constexpr const char* kTypeName`.
template <class Derived>
class NativeClass : public Object {
public:
    ValueKind kind() const override { return ValueKind::Instance; }
    std::string typeName() const override { return Derived::kTypeName; }
    bool truthy() const override { return true; }
    std::string str(StringifyCtx&) const override {
        return std::string("<") + Derived::kTypeName + ">";
    }
    bool equals(const ObjectArena&, const Object& other) const override { return this == &other; }
};

// Wrap a member function's positional implementation so it ALSO accepts keyword arguments, without
// touching the impl. `params` names the positional slots. On a keyword call,
// positionals fill left-to-right, keywords bind by name, any slot left as a hole before the last
// supplied one is filled with None, and trailing unset slots are dropped — so the impl receives
// exactly the variable-length span it always did (its own None/arity checks still apply). A
// positional-only call takes a fast path identical to the original. Unknown/duplicate keywords throw
// a clear error. Used to give every built-in type method and native-class method keyword support.
inline Handle makeMethod(KiritoVM& vm, std::string name, std::vector<std::string> params,
                         NativeFn impl, std::vector<Handle> captures = {}, std::size_t minArgs = 0) {
    NativeFnKw kw = [name, params, impl, minArgs](KiritoVM& v, std::span<const Handle> pos,
                                                  std::span<const NamedArg> named) -> Handle {
        if (named.empty()) return impl(v, pos);  // positional fast path: unchanged behaviour
        std::size_t nparams = params.size();
        std::size_t total = std::max(nparams, pos.size());
        std::vector<Handle> slots(total);
        std::vector<bool> set(total, false);
        for (std::size_t i = 0; i < pos.size(); ++i) { slots[i] = pos[i]; set[i] = true; }
        for (const auto& na : named) {
            std::size_t idx = nparams;
            for (std::size_t i = 0; i < nparams; ++i) if (params[i] == na.name) { idx = i; break; }
            if (idx == nparams)
                throw KiritoError(name + "() got an unexpected keyword argument '" + na.name + "'");
            if (set[idx])
                throw KiritoError(name + "() got multiple values for argument '" + na.name + "'");
            slots[idx] = na.value; set[idx] = true;
        }
        std::size_t outlen = 0;
        for (std::size_t i = 0; i < total; ++i) if (set[i]) outlen = i + 1;
        // A hole before the last supplied arg is filled with None (the impl's default handling) —
        // EXCEPT a hole in a REQUIRED leading slot (index < minArgs), which means the caller named a
        // later arg but omitted a required one (`d.setdefault(default=7)` skipping `key`). None-filling
        // it silently passed None as that argument (inserting {None: 7}); instead this is a clear error.
        for (std::size_t i = 0; i < outlen; ++i) {
            if (set[i]) continue;
            if (i < minArgs)
                throw KiritoError(name + "() missing required argument '" +
                                  (i < params.size() ? params[i] : std::to_string(i)) + "'");
            slots[i] = v.none();
        }
        slots.resize(outlen);
        return impl(v, slots);
    };
    return vm.alloc(std::make_unique<NativeFunction>(std::move(name), std::move(kw), std::move(captures)));
}

}  // namespace kirito

#endif
