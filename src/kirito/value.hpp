#ifndef KIRITO_VALUE_HPP
#define KIRITO_VALUE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "builtins.hpp"
#include "collections.hpp"
#include "object.hpp"
#include "vm.hpp"

// ================================================================================================
// The ergonomic C++ facade for Kirito's built-in types. A native module reads like Kirito itself:
// peek what an argument is (`arg.isDict()`), wrap it (`Dict d = arg.asDict()`), then operate with
// methods and operators — `d["k"]`, `d.contains(k)`, `s[i]` code-point indexing, `xs.push(v)`,
// `for (auto v : xs) …`.
//
// Layering. Every wrapper is a THIN VIEW over `(KiritoVM&, Handle)`. Copy/assign is O(1) and never
// allocates; "wrapping" (`args[0].asDict()`) is a kind check + a re-hosted (vm, handle) — no copy
// of the underlying data. The underlying `Object` stays owned by the VM's arena and rooted by the
// usual GC machinery; mutations go through the low-layer `ListVal`/`DictVal`/`SetVal` API which
// already roots intermediates on the temp-roots stack.
//
// Construction. No free functions: constructors do the work.
//   `Value(vm, 42)`, `Value(vm, "hi")`, `Value::None(vm)` — primitive → Value.
//   `String(vm, "hi")`, `Bytes(vm, raw)` — typed wrappers over a fresh allocation.
//   `List(vm, {1, "a", 3.14})`, `Dict(vm, {{"k", 1}, {"m", "s"}})`, `Set(vm, {1, 2, 3})` — from
//   initialiser lists mixing any C++ primitive or an existing Value/Handle.
// ================================================================================================

namespace kirito {

class Value;
class String;
class Bytes;
class List;
class Dict;
class Set;
class Args;

namespace detail {

// A single value constructible from any C++ primitive or an existing Value/Handle. Powers the
// `initializer_list<Anything>` overloads so `List(vm, {1, "hi", 3.14})` and
// `Dict(vm, {{"k", 1}, {"n", "x"}})` mix types freely.
struct Anything {
    Handle h;
    Anything(Handle x) : h(x) {}
    Anything(const Value& v);                                    // defined below (needs Value)
    Anything(KiritoVM& vm, std::nullptr_t) : h(vm.none()) {}
    Anything(KiritoVM& vm, bool b) : h(vm.makeBool(b)) {}
    Anything(KiritoVM& vm, int v) : h(vm.makeInt(static_cast<int64_t>(v))) {}
    Anything(KiritoVM& vm, unsigned v) : h(vm.makeInt(static_cast<int64_t>(v))) {}
    Anything(KiritoVM& vm, long v) : h(vm.makeInt(static_cast<int64_t>(v))) {}
    Anything(KiritoVM& vm, long long v) : h(vm.makeInt(static_cast<int64_t>(v))) {}
    Anything(KiritoVM& vm, unsigned long v) : h(vm.makeInt(static_cast<int64_t>(v))) {}
    Anything(KiritoVM& vm, unsigned long long v) : h(vm.makeInt(static_cast<int64_t>(v))) {}
    Anything(KiritoVM& vm, float v) : h(vm.makeFloat(static_cast<double>(v))) {}
    Anything(KiritoVM& vm, double v) : h(vm.makeFloat(v)) {}
    Anything(KiritoVM& vm, const char* s) : h(vm.makeString(std::string(s))) {}
    Anything(KiritoVM& vm, std::string s) : h(vm.makeString(std::move(s))) {}
    Anything(KiritoVM& vm, std::string_view s) : h(vm.makeString(std::string(s))) {}
};

}  // namespace detail

// ================================================================================================
// Value — the polymorphic base view over `(KiritoVM&, Handle)`. All typed wrappers inherit from it.
// A default-constructed Value is empty (`vm_ == nullptr`); accessors on an empty Value throw.
// ================================================================================================
class Value {
public:
    Value() = default;
    Value(KiritoVM& vm, Handle h) : vm_(&vm), h_(h) {}

    // primitive constructors — replace the old free `val(vm, x)` / `none(vm)` helpers.
    Value(KiritoVM& vm, std::nullptr_t) : vm_(&vm), h_(vm.none()) {}
    Value(KiritoVM& vm, bool b) : vm_(&vm), h_(vm.makeBool(b)) {}
    Value(KiritoVM& vm, int v) : vm_(&vm), h_(vm.makeInt(static_cast<int64_t>(v))) {}
    Value(KiritoVM& vm, unsigned v) : vm_(&vm), h_(vm.makeInt(static_cast<int64_t>(v))) {}
    Value(KiritoVM& vm, long v) : vm_(&vm), h_(vm.makeInt(static_cast<int64_t>(v))) {}
    Value(KiritoVM& vm, long long v) : vm_(&vm), h_(vm.makeInt(static_cast<int64_t>(v))) {}
    Value(KiritoVM& vm, unsigned long v) : vm_(&vm), h_(vm.makeInt(static_cast<int64_t>(v))) {}
    Value(KiritoVM& vm, unsigned long long v) : vm_(&vm), h_(vm.makeInt(static_cast<int64_t>(v))) {}
    Value(KiritoVM& vm, double v) : vm_(&vm), h_(vm.makeFloat(v)) {}
    Value(KiritoVM& vm, const char* s) : vm_(&vm), h_(vm.makeString(std::string(s))) {}
    Value(KiritoVM& vm, std::string s) : vm_(&vm), h_(vm.makeString(std::move(s))) {}
    Value(KiritoVM& vm, std::string_view s) : vm_(&vm), h_(vm.makeString(std::string(s))) {}

    // Kirito's `None` — the natural spelling. Static so it reads as a factory, not a value.
    static Value None(KiritoVM& vm) { return Value(vm, vm.none()); }

    // implicit → Handle for interop with the raw protocol.
    operator Handle() const { return h_; }
    Handle handle() const { return h_; }
    KiritoVM& vm() const { requireBound(); return *vm_; }
    bool bound() const { return vm_ != nullptr; }

    ValueKind kind() const { return ref().kind(); }
    std::string typeName() const { return ref().typeName(); }
    bool truthy() const { return ref().truthy(); }
    std::string str() const { return vm_->stringify(h_); }

    // Kind predicates — cheap peeks that let a native function pick a branch before wrapping.
    bool isNone() const { return kind() == ValueKind::None; }
    bool isBool() const { return kind() == ValueKind::Bool; }
    bool isInt() const { return kind() == ValueKind::Integer; }
    bool isFloat() const { return kind() == ValueKind::Float; }
    bool isNumber() const { return isInt() || isFloat(); }
    bool isString() const { return kind() == ValueKind::String; }
    bool isBytes() const { return kind() == ValueKind::Instance && typeName() == "Bytes"; }
    bool isList() const { return kind() == ValueKind::List || kind() == ValueKind::Array; }
    bool isDict() const { return kind() == ValueKind::Dict; }
    bool isSet() const { return kind() == ValueKind::Set; }

    // Typed reads for scalars. `who` names the caller for a clear type-mismatch error.
    int64_t asInt(const char* who = "value") const {
        if (!isInt()) typeError(who, "Integer");
        return static_cast<const IntVal&>(ref()).value();
    }
    double asFloat(const char* who = "value") const {                // accepts Integer or Float
        if (isInt()) return static_cast<double>(static_cast<const IntVal&>(ref()).value());
        if (isFloat()) return static_cast<const FloatVal&>(ref()).value();
        typeError(who, "a number");
    }
    bool asBool(const char* who = "value") const {
        if (!isBool()) typeError(who, "Bool");
        return static_cast<const BoolVal&>(ref()).value();
    }
    // Raw UTF-8 bytes of a String. Zero-copy: returns a reference into the arena-owned StrVal.
    const std::string& asStringRef(const char* who = "value") const {
        if (!isString()) typeError(who, "String");
        return static_cast<const StrVal&>(ref()).value();
    }

    // Peek + wrap. Throw on wrong kind; use `tryString()` etc. for optional wrapping.
    String asString(const char* who = "value") const;
    Bytes asBytes(const char* who = "value") const;
    List asList(const char* who = "value") const;
    Dict asDict(const char* who = "value") const;
    Set asSet(const char* who = "value") const;

    std::optional<String> tryString() const;
    std::optional<Bytes> tryBytes() const;
    std::optional<List> tryList() const;
    std::optional<Dict> tryDict() const;
    std::optional<Set> trySet() const;

    // Length across any type that has one (String/List/Dict/Set/Bytes/…). Throws if none.
    std::size_t len() const {
        auto n = ref().length(*vm_);
        if (!n) throw KiritoError("type '" + typeName() + "' has no length");
        return static_cast<std::size_t>(*n);
    }

    // Iterate any iterable into Values (list elements, string code-points, dict keys, …).
    std::vector<Value> items() const {
        auto it = ref().iterate(*vm_);
        if (!it) throw KiritoError("type '" + typeName() + "' is not iterable");
        std::vector<Value> out;
        out.reserve(it->size());
        for (Handle e : *it) out.emplace_back(*vm_, e);
        return out;
    }

    // Structural equality via the value protocol.
    bool equals(const Value& other) const {
        return ref().equals(vm_->arena(), other.ref());
    }

    // Compatibility helpers — delegate to the appropriate typed wrapper. Definitions after Dict.
    std::vector<std::pair<Value, Value>> pairs() const;
    Value at(std::ptrdiff_t i) const;
    bool has(std::string_view k) const;
    Value get(std::string_view k) const;
    Value get(std::string_view k, Value dflt) const;

    // Call this Value as a callable, forwarding positional handles.
    Value call(std::span<const Handle> args) const {
        return Value(*vm_, ref().call(*vm_, args));
    }
    // Convenience: call with primitives / Values captured in an initializer list.
    Value call(std::initializer_list<detail::Anything> args) const {
        std::vector<Handle> hs;
        hs.reserve(args.size());
        for (const auto& a : args) hs.push_back(a.h);
        return call(std::span<const Handle>(hs));
    }

    // Attribute access, matching the object protocol.
    Value getAttr(std::string_view name) const {
        return Value(*vm_, ref().getAttr(*vm_, h_, name));
    }
    void setAttr(std::string_view name, const Value& v) const {
        ref().setAttr(*vm_, name, v.h_);
    }

protected:
    Object& ref() const { requireBound(); return vm_->arena().deref(h_); }
    [[noreturn]] void typeError(const char* who, const char* want) const {
        throw KiritoError(std::string(who) + " expected " + want + ", got '" + typeName() + "'");
    }
    void requireBound() const {
        if (!vm_) throw KiritoError("uninitialised Value (no VM)");
    }
    KiritoVM* vm_ = nullptr;
    Handle h_{};
};

// ================================================================================================
// String — Unicode (code-point) text. `s[i]` returns a 1-code-point String (matches Kirito), `size()`
// counts code points, `utf8()` gets the raw byte string.
// ================================================================================================
class String : public Value {
public:
    String() = default;
    // Wrap an existing handle. `who` labels the type-mismatch error.
    String(KiritoVM& vm, Handle h, const char* who = "String") {
        vm_ = &vm; h_ = h;
        if (!isString()) typeError(who, "String");
    }
    // Fresh String from raw UTF-8.
    explicit String(KiritoVM& vm, std::string_view utf8) {
        vm_ = &vm; h_ = vm.makeString(std::string(utf8));
    }
    explicit String(KiritoVM& vm, const char* utf8) {
        vm_ = &vm; h_ = vm.makeString(std::string(utf8));
    }
    explicit String(KiritoVM& vm, std::string utf8) {
        vm_ = &vm; h_ = vm.makeString(std::move(utf8));
    }

    // Raw UTF-8 bytes — zero-copy reference into the arena.
    const std::string& utf8() const { return static_cast<const StrVal&>(ref()).value(); }
    // Convenience alias for callers that want the "string value" wording.
    const std::string& value() const { return utf8(); }
    // Code-point count (Kirito's `len(s)`).
    std::size_t size() const {
        const auto& s = static_cast<const StrVal&>(ref());
        return s.isAscii() ? s.value().size() : s.codePointStarts().size();
    }
    bool empty() const { return utf8().empty(); }

    // Code-point indexing. Negative indexes count from the end (Kirito semantics). Returns a fresh
    // 1-code-point String. Throws on out-of-range.
    String operator[](std::ptrdiff_t i) const {
        const auto& s = static_cast<const StrVal&>(ref());
        std::ptrdiff_t n = static_cast<std::ptrdiff_t>(size());
        std::ptrdiff_t k = i < 0 ? i + n : i;
        if (k < 0 || k >= n) throw KiritoError("String index out of range");
        if (s.isAscii()) return String(*vm_, std::string(1, s.value()[static_cast<std::size_t>(k)]));
        const auto& starts = s.codePointStarts();
        std::size_t a = starts[static_cast<std::size_t>(k)];
        std::size_t b = static_cast<std::size_t>(k) + 1 < starts.size()
                         ? starts[static_cast<std::size_t>(k) + 1]
                         : s.value().size();
        return String(*vm_, s.value().substr(a, b - a));
    }

    // Substring by byte-safe code-point search (Kirito's `s.contains(sub)`).
    bool contains(std::string_view sub) const { return utf8().find(sub) != std::string::npos; }
    bool startsWith(std::string_view p) const {
        const auto& s = utf8();
        return s.size() >= p.size() && std::string_view(s).substr(0, p.size()) == p;
    }
    bool endsWith(std::string_view p) const {
        const auto& s = utf8();
        return s.size() >= p.size() &&
               std::string_view(s).substr(s.size() - p.size()) == p;
    }

    // Concatenation. Returns a fresh String.
    String operator+(const String& rhs) const { return String(*vm_, utf8() + rhs.utf8()); }

    // Byte-exact equality with a raw literal — handy in tests (`s == "hi"`).
    bool operator==(std::string_view rhs) const { return utf8() == rhs; }
    bool operator==(const char* rhs) const { return utf8() == std::string_view(rhs); }
    bool operator!=(std::string_view rhs) const { return !(*this == rhs); }
    bool operator!=(const char* rhs) const { return !(*this == rhs); }
    // Implicit conversion to std::string_view for painless interop with std helpers.
    operator std::string_view() const { return utf8(); }
};

// ================================================================================================
// Bytes — immutable byte sequence. `b[i]` returns the byte as an int (0..255).
// ================================================================================================
class Bytes : public Value {
public:
    Bytes() = default;
    Bytes(KiritoVM& vm, Handle h, const char* who = "Bytes") {
        vm_ = &vm; h_ = h;
        if (!isBytes()) typeError(who, "Bytes");
    }
    // Fresh Bytes from raw bytes (any string_view of bytes). Defined out-of-line in bytes.hpp,
    // where the BytesVal layout is visible.
    explicit Bytes(KiritoVM& vm, std::string_view raw);
    explicit Bytes(KiritoVM& vm, std::string raw);

    // Raw byte access — zero-copy reference to the underlying std::string buffer.
    // Defined in bytes.hpp.
    const std::string& data() const;
    std::size_t size() const { return data().size(); }
    bool empty() const { return data().empty(); }

    // Byte at index — returns 0..255 as int. Negative indexes count from the end.
    int operator[](std::ptrdiff_t i) const {
        const auto& d = data();
        std::ptrdiff_t n = static_cast<std::ptrdiff_t>(d.size());
        std::ptrdiff_t k = i < 0 ? i + n : i;
        if (k < 0 || k >= n) throw KiritoError("Bytes index out of range");
        return static_cast<unsigned char>(d[static_cast<std::size_t>(k)]);
    }
};

// ================================================================================================
// List — ordered sequence. `xs[i]` reads, `xs.push(v)` appends, `xs.contains(v)` searches by value.
// Zero-alloc peek/wrap; mutating methods forward to the underlying `ListVal`.
// ================================================================================================
class List : public Value {
public:
    List() = default;
    // Wrap an existing List/Array handle.
    List(KiritoVM& vm, Handle h, const char* who = "List") {
        vm_ = &vm; h_ = h;
        if (!isList()) typeError(who, "List");
    }
    // Fresh empty List.
    explicit List(KiritoVM& vm) {
        vm_ = &vm; h_ = vm.alloc(std::make_unique<ListVal>());
    }
    // Fresh List from an initializer list of any-type items.
    List(KiritoVM& vm, std::initializer_list<detail::Anything> items) {
        vm_ = &vm;
        RootScope roots(vm);
        auto lv = std::make_unique<ListVal>();
        lv->elems.reserve(items.size());
        for (const auto& a : items) { roots.add(a.h); lv->elems.push_back(a.h); }
        h_ = vm.alloc(std::move(lv));
    }

    std::size_t size() const { return raw().elems.size(); }
    bool empty() const { return raw().elems.empty(); }

    // Element access with Kirito-style negative indexing.
    Value operator[](std::ptrdiff_t i) const {
        const auto& e = raw().elems;
        std::ptrdiff_t n = static_cast<std::ptrdiff_t>(e.size());
        std::ptrdiff_t k = i < 0 ? i + n : i;
        if (k < 0 || k >= n) throw KiritoError("List index out of range");
        return Value(*vm_, e[static_cast<std::size_t>(k)]);
    }

    // Overwrite (in-place). Uses positive index; negatives supported symmetrically.
    void set(std::ptrdiff_t i, const Value& v) {
        auto& e = mut().elems;
        std::ptrdiff_t n = static_cast<std::ptrdiff_t>(e.size());
        std::ptrdiff_t k = i < 0 ? i + n : i;
        if (k < 0 || k >= n) throw KiritoError("List index out of range");
        e[static_cast<std::size_t>(k)] = v.handle();
    }

    // Append. Chainable for fluent construction.
    List& push(const Value& v) { mut().elems.push_back(v.handle()); return *this; }
    List& push(Handle h) { mut().elems.push_back(h); return *this; }
    template <class T> List& push(T x) { return push(Value(*vm_, x)); }

    // Compatibility aliases for callers migrating from the old builder API.
    List& add(const Value& v) { return push(v); }
    List& add(Handle h) { return push(h); }
    template <class T> List& add(T x) { return push(x); }
    Value build() { return *this; }                                  // no-op: already a Value view

    // Pop and return the last element.
    Value pop() {
        auto& e = mut().elems;
        if (e.empty()) throw KiritoError("pop from empty list");
        Handle h = e.back();
        e.pop_back();
        return Value(*vm_, h);
    }

    // Membership by value-protocol equality.
    bool contains(const Value& v) const {
        const auto& e = raw().elems;
        for (Handle h : e)
            if (vm_->arena().deref(h).equals(vm_->arena(), vm_->arena().deref(v.handle())))
                return true;
        return false;
    }

    void clear() { mut().elems.clear(); }

    // Iteration — range-for over Values.
    class Iterator {
    public:
        Iterator(KiritoVM* vm, const std::vector<Handle>* e, std::size_t i) : vm_(vm), e_(e), i_(i) {}
        Value operator*() const { return Value(*vm_, (*e_)[i_]); }
        Iterator& operator++() { ++i_; return *this; }
        bool operator!=(const Iterator& o) const { return i_ != o.i_; }
    private:
        KiritoVM* vm_;
        const std::vector<Handle>* e_;
        std::size_t i_;
    };
    Iterator begin() const { return Iterator(vm_, &raw().elems, 0); }
    Iterator end() const { return Iterator(vm_, &raw().elems, raw().elems.size()); }

private:
    const ListVal& raw() const { return static_cast<const ListVal&>(ref()); }
    ListVal& mut() { return static_cast<ListVal&>(ref()); }
};

// ================================================================================================
// Dict — key/value map. `d[k]` reads (throws if missing), `d.get(k, dflt)` reads with default,
// `d.set(k, v)` writes chainably, `d.contains(k)`/`d.has(k)` peek.
// ================================================================================================
class Dict : public Value {
public:
    Dict() = default;
    Dict(KiritoVM& vm, Handle h, const char* who = "Dict") {
        vm_ = &vm; h_ = h;
        if (!isDict()) typeError(who, "Dict");
    }
    explicit Dict(KiritoVM& vm) {
        vm_ = &vm; h_ = vm.alloc(std::make_unique<DictVal>());
    }
    Dict(KiritoVM& vm, std::initializer_list<
             std::pair<detail::Anything, detail::Anything>> entries) {
        vm_ = &vm;
        RootScope roots(vm);
        auto dv = std::make_unique<DictVal>();
        for (const auto& [k, v] : entries) {
            roots.add(k.h); roots.add(v.h);
            dv->set(vm.arena(), k.h, v.h);
        }
        h_ = vm.alloc(std::move(dv));
    }

    std::size_t size() const { return raw().count; }
    bool empty() const { return raw().count == 0; }

    // Read by key. Throws if the key is absent.
    Value operator[](const Value& k) const { return at(k); }
    Value operator[](std::string_view sk) const { return at(Value(*vm_, sk)); }
    Value at(const Value& k) const {
        const Handle* v = raw().find(vm_->arena(), k.handle());
        if (!v) throw KiritoError("Dict has no key '" + k.str() + "'");
        return Value(*vm_, *v);
    }

    // Read by key with a default (returned as-is when absent). No allocation on hit.
    Value get(const Value& k, Value dflt) const {
        const Handle* v = raw().find(vm_->arena(), k.handle());
        return v ? Value(*vm_, *v) : dflt;
    }
    Value get(std::string_view sk, Value dflt) const { return get(Value(*vm_, sk), dflt); }

    // Optional read.
    std::optional<Value> tryGet(const Value& k) const {
        const Handle* v = raw().find(vm_->arena(), k.handle());
        return v ? std::optional<Value>(Value(*vm_, *v)) : std::nullopt;
    }
    std::optional<Value> tryGet(std::string_view sk) const { return tryGet(Value(*vm_, sk)); }

    // Write. Chainable.
    Dict& set(const Value& k, const Value& v) {
        RootScope roots(*vm_);
        roots.add(k.handle()); roots.add(v.handle());
        mut().set(vm_->arena(), k.handle(), v.handle());
        return *this;
    }
    Dict& set(std::string_view sk, const Value& v) { return set(Value(*vm_, sk), v); }
    Dict& set(std::string_view sk, Handle h) { return set(Value(*vm_, sk), Value(*vm_, h)); }
    template <class V> Dict& set(std::string_view sk, V v) {
        return set(Value(*vm_, sk), Value(*vm_, v));
    }
    template <class K, class V> Dict& set(K k, V v) {
        return set(Value(*vm_, k), Value(*vm_, v));
    }
    // Overloads accepting a raw Handle key (compat).
    Dict& set(Handle k, Handle v) { return set(Value(*vm_, k), Value(*vm_, v)); }
    Dict& set(Handle k, const Value& v) { return set(Value(*vm_, k), v); }

    // Compatibility: `build()` — the Dict is already a Value view.
    Value build() { return *this; }

    // Membership check.
    bool contains(const Value& k) const { return raw().find(vm_->arena(), k.handle()) != nullptr; }
    bool contains(std::string_view sk) const { return contains(Value(*vm_, sk)); }
    // Kirito-style alias.
    bool has(const Value& k) const { return contains(k); }
    bool has(std::string_view sk) const { return contains(sk); }

    // Remove a key. Returns true if it was present.
    bool remove(const Value& k) { return mut().remove(vm_->arena(), k.handle()); }
    bool remove(std::string_view sk) { return remove(Value(*vm_, sk)); }

    void clear() { auto& d = mut(); d.buckets.clear(); d.count = 0; }

    // Key list (hash-bucket order — same order as Kirito's iteration).
    std::vector<Value> keys() const {
        std::vector<Value> out;
        out.reserve(size());
        for (Handle k : raw().keys()) out.emplace_back(*vm_, k);
        return out;
    }
    std::vector<Value> values() const {
        std::vector<Value> out;
        out.reserve(size());
        for (const auto& [k, v] : raw().pairs()) { (void)k; out.emplace_back(*vm_, v); }
        return out;
    }
    std::vector<std::pair<Value, Value>> pairs() const {
        std::vector<std::pair<Value, Value>> out;
        out.reserve(size());
        for (const auto& [k, v] : raw().pairs()) out.emplace_back(Value(*vm_, k), Value(*vm_, v));
        return out;
    }

    // Range-for support: for (auto [k, v] : d) …
    class Iterator {
    public:
        Iterator(KiritoVM* vm, std::vector<std::pair<Handle, Handle>> ps, std::size_t i)
            : vm_(vm), ps_(std::move(ps)), i_(i) {}
        std::pair<Value, Value> operator*() const {
            return {Value(*vm_, ps_[i_].first), Value(*vm_, ps_[i_].second)};
        }
        Iterator& operator++() { ++i_; return *this; }
        bool operator!=(const Iterator& o) const { return i_ != o.i_; }
    private:
        KiritoVM* vm_;
        std::vector<std::pair<Handle, Handle>> ps_;
        std::size_t i_;
    };
    Iterator begin() const { return Iterator(vm_, raw().pairs(), 0); }
    Iterator end() const {
        auto ps = raw().pairs();
        auto n = ps.size();
        return Iterator(vm_, std::move(ps), n);
    }

private:
    const DictVal& raw() const { return static_cast<const DictVal&>(ref()); }
    DictVal& mut() { return static_cast<DictVal&>(ref()); }
};

// ================================================================================================
// Set — unordered unique values. `s.add(v)` inserts, `s.contains(v)` peeks, `s.discard(v)` removes
// silently. Ordering follows the hash-bucket walk (Kirito iteration order).
// ================================================================================================
class Set : public Value {
public:
    Set() = default;
    Set(KiritoVM& vm, Handle h, const char* who = "Set") {
        vm_ = &vm; h_ = h;
        if (!isSet()) typeError(who, "Set");
    }
    explicit Set(KiritoVM& vm) {
        vm_ = &vm; h_ = vm.alloc(std::make_unique<SetVal>());
    }
    Set(KiritoVM& vm, std::initializer_list<detail::Anything> items) {
        vm_ = &vm;
        RootScope roots(vm);
        auto sv = std::make_unique<SetVal>();
        for (const auto& a : items) { roots.add(a.h); sv->add(vm.arena(), a.h); }
        h_ = vm.alloc(std::move(sv));
    }

    std::size_t size() const { return raw().count; }
    bool empty() const { return raw().count == 0; }

    Set& add(const Value& v) {
        RootScope roots(*vm_);
        roots.add(v.handle());
        mut().add(vm_->arena(), v.handle());
        return *this;
    }
    template <class T> Set& add(T x) { return add(Value(*vm_, x)); }
    Set& add(Handle h) { return add(Value(*vm_, h)); }

    // Compatibility: `build()` — the Set is already a Value view.
    Value build() { return *this; }

    bool contains(const Value& v) const { return raw().contains(vm_->arena(), v.handle()); }
    template <class T> bool contains(T x) const { return contains(Value(*vm_, x)); }

    // Remove; silent if absent. Rebuilds without the value since SetVal has no direct remove.
    void discard(const Value& v) {
        auto& s = mut();
        if (!s.contains(vm_->arena(), v.handle())) return;
        SetVal rebuilt;
        for (Handle h : s.items())
            if (!vm_->arena().deref(h).equals(vm_->arena(), vm_->arena().deref(v.handle())))
                rebuilt.add(vm_->arena(), h);
        s.buckets = std::move(rebuilt.buckets);
        s.count = rebuilt.count;
    }

    void clear() { auto& s = mut(); s.buckets.clear(); s.count = 0; }

    std::vector<Value> items() const {
        std::vector<Value> out;
        out.reserve(size());
        for (Handle h : raw().items()) out.emplace_back(*vm_, h);
        return out;
    }

    class Iterator {
    public:
        Iterator(KiritoVM* vm, std::vector<Handle> hs, std::size_t i)
            : vm_(vm), hs_(std::move(hs)), i_(i) {}
        Value operator*() const { return Value(*vm_, hs_[i_]); }
        Iterator& operator++() { ++i_; return *this; }
        bool operator!=(const Iterator& o) const { return i_ != o.i_; }
    private:
        KiritoVM* vm_;
        std::vector<Handle> hs_;
        std::size_t i_;
    };
    Iterator begin() const { return Iterator(vm_, raw().items(), 0); }
    Iterator end() const {
        auto is = raw().items();
        auto n = is.size();
        return Iterator(vm_, std::move(is), n);
    }

private:
    const SetVal& raw() const { return static_cast<const SetVal&>(ref()); }
    SetVal& mut() { return static_cast<SetVal&>(ref()); }
};

// ================================================================================================
// Args — positional-argument view for a native function's incoming span.
//   `Args a(vm, span, "open");`
//   `a[0].asString()`, `a.opt(1, Value(vm, "r"))`, `a.at(2)` (throws with the fn name).
// ================================================================================================
class Args {
public:
    Args(KiritoVM& vm, std::span<const Handle> a, const char* fn = "function")
        : vm_(&vm), a_(a), fn_(fn) {}
    std::size_t size() const { return a_.size(); }
    bool empty() const { return a_.empty(); }
    Value operator[](std::size_t i) const { return Value(*vm_, a_[i]); }
    Value at(std::size_t i) const {
        if (i >= a_.size())
            throw KiritoError(std::string(fn_) + " missing argument " + std::to_string(i + 1));
        return (*this)[i];
    }
    Value opt(std::size_t i, Value dflt) const { return i < a_.size() ? (*this)[i] : dflt; }
    // Raw underlying span, for delegating to the low-layer protocol.
    std::span<const Handle> raw() const { return a_; }

    // Enforce a minimum arity with a clean message.
    void require(std::size_t n) const {
        if (a_.size() < n)
            throw KiritoError(std::string(fn_) + " expects " + std::to_string(n) +
                              " arguments, got " + std::to_string(a_.size()));
    }
private:
    KiritoVM* vm_;
    std::span<const Handle> a_;
    const char* fn_;
};

// ================================================================================================
// Deferred definitions — the typed wrappers, `Anything::Anything(const Value&)`, and the Bytes
// helpers that depend on the BytesVal layout.
// ================================================================================================

inline detail::Anything::Anything(const Value& v) : h(v.handle()) {}

inline String Value::asString(const char* who) const { return String(*vm_, h_, who); }
inline List   Value::asList(const char* who)   const { return List(*vm_, h_, who); }
inline Dict   Value::asDict(const char* who)   const { return Dict(*vm_, h_, who); }
inline Set    Value::asSet(const char* who)    const { return Set(*vm_, h_, who); }
inline Bytes  Value::asBytes(const char* who)  const { return Bytes(*vm_, h_, who); }

inline std::optional<String> Value::tryString() const {
    return isString() ? std::optional<String>(String(*vm_, h_)) : std::nullopt;
}
inline std::optional<List> Value::tryList() const {
    return isList() ? std::optional<List>(List(*vm_, h_)) : std::nullopt;
}
inline std::optional<Dict> Value::tryDict() const {
    return isDict() ? std::optional<Dict>(Dict(*vm_, h_)) : std::nullopt;
}
inline std::optional<Set> Value::trySet() const {
    return isSet() ? std::optional<Set>(Set(*vm_, h_)) : std::nullopt;
}
inline std::optional<Bytes> Value::tryBytes() const {
    return isBytes() ? std::optional<Bytes>(Bytes(*vm_, h_)) : std::nullopt;
}

// Value::at — negative-index element access via the object protocol (works for List/String/etc).
inline Value Value::at(std::ptrdiff_t i) const {
    Handle key = vm_->makeInt(static_cast<int64_t>(i));
    std::array<Handle, 1> keys{key};
    return Value(*vm_, ref().getItem(*vm_, keys));
}
inline std::vector<std::pair<Value, Value>> Value::pairs() const { return asDict("pairs").pairs(); }
inline bool Value::has(std::string_view k) const { return asDict("has").contains(k); }
inline Value Value::get(std::string_view k) const { return asDict("get").at(Value(*vm_, k)); }
inline Value Value::get(std::string_view k, Value dflt) const { return asDict("get").get(k, dflt); }

}  // namespace kirito

#endif
