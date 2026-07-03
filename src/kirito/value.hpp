#ifndef KIRITO_VALUE_HPP
#define KIRITO_VALUE_HPP

#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <array>
#include <utility>
#include <vector>

#include "builtins.hpp"
#include "collections.hpp"
#include "object.hpp"
#include "vm.hpp"

// Ergonomic C++ access to Kirito's built-in types. When writing a native module, prefer these over
// raw Handle gymnastics: read arguments with a `Value` facade, build results with the `List`/`Dict`/
// `Set` builders (which root intermediates for the GC automatically), and lean on the built-in types
// instead of defining a new NativeClass — that should be the fallback, only for genuinely new
// behaviour. `Value` converts implicitly to/from `Handle`, so it interoperates with everything.

namespace kirito {

// A lightweight (KiritoVM&, Handle) view with typed accessors. Cheap to copy; holds no ownership.
class Value {
public:
    Value() : vm_(nullptr) {}
    Value(KiritoVM& vm, Handle h) : vm_(&vm), h_(h) {}

    operator Handle() const { return h_; }            // implicit -> Handle for protocol interop
    Handle handle() const { return h_; }
    KiritoVM& vm() const { return *vm_; }

    ValueKind kind() const { return ref().kind(); }
    std::string typeName() const { return ref().typeName(); }
    bool truthy() const { return ref().truthy(); }
    std::string str() const { return vm_->stringify(h_); }

    bool isInt() const { return kind() == ValueKind::Integer; }
    bool isFloat() const { return kind() == ValueKind::Float; }
    bool isNumber() const { return isInt() || isFloat(); }
    bool isString() const { return kind() == ValueKind::String; }
    bool isBool() const { return kind() == ValueKind::Bool; }
    bool isNone() const { return kind() == ValueKind::None; }
    bool isList() const { return kind() == ValueKind::List || kind() == ValueKind::Array; }
    bool isDict() const { return kind() == ValueKind::Dict; }
    bool isSet() const { return kind() == ValueKind::Set; }

    // Typed reads. `who` names the caller for a clear error message on a type mismatch.
    int64_t asInt(const char* who = "value") const {
        if (!isInt()) typeError(who, "Integer");
        return static_cast<const IntVal&>(ref()).value();
    }
    double asFloat(const char* who = "value") const {                // accepts Integer or Float
        if (isInt()) return static_cast<double>(static_cast<const IntVal&>(ref()).value());
        if (isFloat()) return static_cast<const FloatVal&>(ref()).value();
        typeError(who, "a number");
    }
    const std::string& asString(const char* who = "value") const {
        if (!isString()) typeError(who, "String");
        return static_cast<const StrVal&>(ref()).value();
    }
    bool asBool(const char* who = "value") const {
        if (!isBool()) typeError(who, "Bool");
        return static_cast<const BoolVal&>(ref()).value();
    }

    // --- collections (read) ---
    std::size_t len() const {
        auto n = ref().length(*vm_);
        if (!n) throw KiritoError("type '" + typeName() + "' has no length");
        return static_cast<std::size_t>(*n);
    }
    // index a list (negative indexes from the end) — delegates to the protocol.
    Value at(int64_t i) const {
        Handle key = vm_->makeInt(i);
        std::array<Handle, 1> keys{key};
        return Value(*vm_, ref().getItem(*vm_, keys));
    }
    // iterate any iterable into Values (list elements, string chars, dict keys, …).
    std::vector<Value> items() const {
        auto it = ref().iterate(*vm_);
        if (!it) throw KiritoError("type '" + typeName() + "' is not iterable");
        std::vector<Value> out;
        out.reserve(it->size());
        for (Handle e : *it) out.emplace_back(*vm_, e);
        return out;
    }
    // dict lookup by String key.
    bool has(std::string_view key) const {
        const auto& d = asDictRef("has");
        Handle k = vm_->makeString(std::string(key));
        return d.find(vm_->arena(), k) != nullptr;
    }
    Value get(std::string_view key) const {
        const auto& d = asDictRef("get");
        Handle k = vm_->makeString(std::string(key));
        const Handle* v = d.find(vm_->arena(), k);
        if (!v) throw KiritoError("Dict has no key '" + std::string(key) + "'");
        return Value(*vm_, *v);
    }
    Value get(std::string_view key, Value dflt) const {
        const auto& d = asDictRef("get");
        Handle k = vm_->makeString(std::string(key));
        const Handle* v = d.find(vm_->arena(), k);
        return v ? Value(*vm_, *v) : dflt;
    }
    // dict key/value pairs as Values (in the dict's iteration order — hash-bucket, not insertion).
    std::vector<std::pair<Value, Value>> pairs() const {
        const auto& d = asDictRef("pairs");
        std::vector<std::pair<Value, Value>> out;
        for (const auto& [k, v] : d.pairs()) out.emplace_back(Value(*vm_, k), Value(*vm_, v));
        return out;
    }

private:
    // Non-const because the value protocol (length/getItem/iterate) is non-const; `Value` only holds
    // a pointer to the (non-const) VM, so this is logical constness, not a violation.
    Object& ref() const { return vm_->arena().deref(h_); }
    const DictVal& asDictRef(const char* who) const {
        if (!isDict()) typeError(who, "Dict");
        return static_cast<const DictVal&>(ref());
    }
    [[noreturn]] void typeError(const char* who, const char* want) const {
        throw KiritoError(std::string(who) + " expected " + want + ", got '" + typeName() + "'");
    }
    KiritoVM* vm_;
    Handle h_{};
};

// --- construct values from C++ primitives --------------------------------------------------------
inline Value val(KiritoVM& vm, int64_t x) { return Value(vm, vm.makeInt(x)); }
inline Value val(KiritoVM& vm, int x) { return Value(vm, vm.makeInt(static_cast<int64_t>(x))); }
inline Value val(KiritoVM& vm, std::size_t x) { return Value(vm, vm.makeInt(static_cast<int64_t>(x))); }
inline Value val(KiritoVM& vm, double x) { return Value(vm, vm.makeFloat(x)); }
inline Value val(KiritoVM& vm, bool x) { return Value(vm, vm.makeBool(x)); }
inline Value val(KiritoVM& vm, const char* s) { return Value(vm, vm.makeString(s)); }
inline Value val(KiritoVM& vm, std::string s) { return Value(vm, vm.makeString(std::move(s))); }
inline Value none(KiritoVM& vm) { return Value(vm, vm.none()); }

// --- builders (root intermediates so a mid-build GC can't reclaim them) --------------------------

// List builder: `List(vm).add(1).add("x").add(some_handle).build()`.
class List {
public:
    explicit List(KiritoVM& vm) : vm_(vm), roots_(vm), list_(std::make_unique<ListVal>()) {}
    List& add(Handle h) { roots_.add(h); list_->elems.push_back(h); return *this; }
    List& add(const Value& v) { return add(v.handle()); }
    template <class T> List& add(T x) { return add(val(vm_, x)); }
    std::size_t size() const { return list_->elems.size(); }
    Value build() { return Value(vm_, vm_.alloc(std::move(list_))); }
private:
    KiritoVM& vm_;
    RootScope roots_;
    std::unique_ptr<ListVal> list_;
};

// Dict builder: `Dict(vm).set("name", "Ada").set("age", 30).build()`.
class Dict {
public:
    explicit Dict(KiritoVM& vm) : vm_(vm), roots_(vm), dict_(std::make_unique<DictVal>()) {}
    Dict& set(Handle key, Handle value) {
        roots_.add(key); roots_.add(value);
        dict_->set(vm_.arena(), key, value);
        return *this;
    }
    Dict& set(std::string_view key, Handle value) { return set(vm_.makeString(std::string(key)), value); }
    Dict& set(std::string_view key, const Value& value) { return set(key, value.handle()); }
    template <class T> Dict& set(std::string_view key, T value) { return set(key, val(vm_, value)); }
    Value build() { return Value(vm_, vm_.alloc(std::move(dict_))); }
private:
    KiritoVM& vm_;
    RootScope roots_;
    std::unique_ptr<DictVal> dict_;
};

// Set builder: `Set(vm).add(1).add(2).build()`.
class Set {
public:
    explicit Set(KiritoVM& vm) : vm_(vm), roots_(vm), set_(std::make_unique<SetVal>()) {}
    Set& add(Handle h) { roots_.add(h); set_->add(vm_.arena(), h); return *this; }
    Set& add(const Value& v) { return add(v.handle()); }
    template <class T> Set& add(T x) { return add(val(vm_, x)); }
    Value build() { return Value(vm_, vm_.alloc(std::move(set_))); }
private:
    KiritoVM& vm_;
    RootScope roots_;
    std::unique_ptr<SetVal> set_;
};

// Build a list straight from handles/Values.
inline Value makeList(KiritoVM& vm, std::initializer_list<Handle> elems) {
    List b(vm);
    for (Handle h : elems) b.add(h);
    return b.build();
}
inline Value makeList(KiritoVM& vm, const std::vector<Handle>& elems) {
    List b(vm);
    for (Handle h : elems) b.add(h);
    return b.build();
}

// --- positional-argument view for native functions ----------------------------------------------
// `Args a(vm, span, "open"); a[0].asString();  a.opt(1, val(vm, "r"))`.
class Args {
public:
    Args(KiritoVM& vm, std::span<const Handle> a, const char* fn = "function") : vm_(&vm), a_(a), fn_(fn) {}
    std::size_t size() const { return a_.size(); }
    bool empty() const { return a_.empty(); }
    Value operator[](std::size_t i) const { return Value(*vm_, a_[i]); }
    Value at(std::size_t i) const {
        if (i >= a_.size()) throw KiritoError(std::string(fn_) + " missing argument " + std::to_string(i + 1));
        return (*this)[i];
    }
    Value opt(std::size_t i, Value dflt) const { return i < a_.size() ? (*this)[i] : dflt; }
private:
    KiritoVM* vm_;
    std::span<const Handle> a_;
    const char* fn_;
};

}  // namespace kirito

#endif
