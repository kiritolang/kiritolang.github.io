#ifndef KIRITO_VALUE_HPP
#define KIRITO_VALUE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "builtins.hpp"
#include "collections.hpp"
#include "common.hpp"                                          // floatClose (numeric .compare)
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

// Forward decls of the shared operation helpers — defined in runtime.hpp. Value's operator
// overloads delegate here so semantics are byte-identical to a Kirito `a + b` / `a < b` (including
// numeric wraparound on Integer overflow, true-division `/`, exact IEEE-754 `==`, and per-type
// dispatch via the object protocol). `BinOp`/`UnOp` themselves come from common.hpp.
Handle applyBinaryOp(KiritoVM& vm, BinOp op, Handle lhs, Handle rhs);
Handle applyUnaryOp(KiritoVM& vm, UnOp op, Handle operand);

class Value;
class Bool;
class Integer;
class Float;
class String;
class Bytes;
class List;
class Dict;
class Set;
class Args;

namespace detail {

// RAII pin/unpin of an arena handle via KiritoVM::pinHandle/unpinHandle. Wrappers that ALLOCATE a
// fresh container (`List(vm)`, `Dict(vm, {…})`, `Value(vm, "hi")`) hand one to their `pin_` member
// so the freshly-allocated object survives any GC triggered by subsequent user code. The pin is a
// `shared_ptr<Pin>` so a copy of the wrapper shares the same root — the last copy to destruct
// releases the pin. Non-allocating wrappers (`Value(vm, existingHandle)`, `Dict d = arg.asDict()`)
// leave `pin_` null: the underlying object is already rooted somewhere the GC can see.
struct Pin {
    KiritoVM* vm;
    Handle h;
    Pin(KiritoVM& v, Handle x) : vm(&v), h(x) { vm->pinHandle(x); }
    ~Pin() { vm->unpinHandle(h); }
    Pin(const Pin&) = delete;
    Pin& operator=(const Pin&) = delete;
};

// A deferred value constructible from any C++ primitive or an existing Value/Handle without a VM.
// Powers `List(vm, {1, "hi", 3.14})` and `Dict(vm, {{"k", 1}, {"n", "x"}})` — the initializer list
// can't see the container's VM, so each item's Handle is materialised lazily inside `toHandle(vm)`
// when the container constructor visits it.
struct Anything {
    std::variant<Handle, int64_t, double, bool, std::string, std::nullptr_t> v;
    Anything(Handle x)                : v(x) {}
    Anything(const Value& val);                                             // defined below
    Anything(std::nullptr_t)          : v(nullptr) {}
    Anything(bool b)                  : v(b) {}
    Anything(int x)                   : v(static_cast<int64_t>(x)) {}
    Anything(unsigned x)              : v(static_cast<int64_t>(x)) {}
    Anything(long x)                  : v(static_cast<int64_t>(x)) {}
    Anything(long long x)             : v(static_cast<int64_t>(x)) {}
    Anything(unsigned long x)         : v(static_cast<int64_t>(x)) {}
    Anything(unsigned long long x)    : v(static_cast<int64_t>(x)) {}
    Anything(float x)                 : v(static_cast<double>(x)) {}
    Anything(double x)                : v(x) {}
    Anything(const char* s)           : v(std::string(s)) {}
    Anything(std::string s)           : v(std::move(s)) {}
    Anything(std::string_view s)      : v(std::string(s)) {}

    // Realise into an arena handle now that a VM is available.
    Handle toHandle(KiritoVM& vm) const {
        return std::visit([&vm](const auto& x) -> Handle {
            using T = std::decay_t<decltype(x)>;
            if      constexpr (std::is_same_v<T, Handle>)         return x;
            else if constexpr (std::is_same_v<T, int64_t>)        return vm.makeInt(x);
            else if constexpr (std::is_same_v<T, double>)         return vm.makeFloat(x);
            else if constexpr (std::is_same_v<T, bool>)           return vm.makeBool(x);
            else if constexpr (std::is_same_v<T, std::string>)    return vm.makeString(x);
            else /* nullptr_t */                                   return vm.none();
        }, v);
    }
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

    // primitive constructors — replace the old free `val(vm, x)` / `none(vm)` helpers. The
    // small-int / Bool / None cases don't allocate (interned singletons), so they skip the pin;
    // Integer / Float / String allocate a fresh boxed value and pin it via adopt() so a mid-
    // expression GC can't sweep the temporary before it's stored somewhere.
    Value(KiritoVM& vm, std::nullptr_t) : vm_(&vm), h_(vm.none()) {}
    Value(KiritoVM& vm, bool b) : vm_(&vm), h_(vm.makeBool(b)) {}
    Value(KiritoVM& vm, int v)                { adopt(vm, vm.makeInt(static_cast<int64_t>(v))); }
    Value(KiritoVM& vm, unsigned v)           { adopt(vm, vm.makeInt(static_cast<int64_t>(v))); }
    Value(KiritoVM& vm, long v)               { adopt(vm, vm.makeInt(static_cast<int64_t>(v))); }
    Value(KiritoVM& vm, long long v)          { adopt(vm, vm.makeInt(static_cast<int64_t>(v))); }
    Value(KiritoVM& vm, unsigned long v)      { adopt(vm, vm.makeInt(static_cast<int64_t>(v))); }
    Value(KiritoVM& vm, unsigned long long v) { adopt(vm, vm.makeInt(static_cast<int64_t>(v))); }
    Value(KiritoVM& vm, double v)             { adopt(vm, vm.makeFloat(v)); }
    Value(KiritoVM& vm, const char* s)        { adopt(vm, vm.makeString(std::string(s))); }
    Value(KiritoVM& vm, std::string s)        { adopt(vm, vm.makeString(std::move(s))); }
    Value(KiritoVM& vm, std::string_view s)   { adopt(vm, vm.makeString(std::string(s))); }

    // Kirito's `None` — the natural spelling. Static so it reads as a factory, not a value.
    static Value None(KiritoVM& vm) { return Value(vm, vm.none()); }

    // Wrap a FRESHLY-allocated handle (an applyBinaryOp / call / getAttr / pop result) and PIN it, so
    // a GC before the Value is first used can't sweep the new object (a dangling handle / UAF). Use
    // this — not the plain Value(vm, h) ctor, which assumes the handle is already rooted elsewhere —
    // whenever the handle is a new allocation not otherwise reachable from a GC root.
    static Value adopting(KiritoVM& vm, Handle fresh) { Value v; v.adopt(vm, fresh); return v; }

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
    bool isBytes() const { return ref().isBytesValue(); }
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
    Bool asBoolV(const char* who = "value") const;                          // "V" -> Value-typed
    Integer asInteger(const char* who = "value") const;
    Float asFloatV(const char* who = "value") const;
    String asString(const char* who = "value") const;
    Bytes asBytes(const char* who = "value") const;
    List asList(const char* who = "value") const;
    Dict asDict(const char* who = "value") const;
    Set asSet(const char* who = "value") const;

    std::optional<Bool> tryBoolV() const;
    std::optional<Integer> tryInteger() const;
    std::optional<Float> tryFloatV() const;
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

    // Iterate any iterable into Values (list elements, string code-points, dict keys, …). Each element
    // is PINNED for the returned vector's lifetime: a String/Bytes iterate yields FRESH per-element
    // boxes rooted only by iterate's internal scope (destroyed on return), so an unpinned view would
    // dangle after the next allocation-triggered GC (A07-4). Pinning is harmless for already-rooted
    // List/Set/Dict elements. No arena allocation happens in the loop, so nothing is swept before the
    // pins are taken.
    std::vector<Value> items() const {
        auto it = ref().iterate(*vm_);
        if (!it) throw KiritoError("type '" + typeName() + "' is not iterable");
        std::vector<Value> out;
        out.reserve(it->size());
        for (Handle e : *it) {
            Value v;
            v.adopt(*vm_, e);
            out.push_back(std::move(v));
        }
        return out;
    }

    // Structural equality via the value protocol (exact — the same predicate Kirito's `==` uses).
    bool equals(const Value& other) const {
        return ref().equals(vm_->arena(), other.ref());
    }

    // Full operator surface — delegates to the shared BinOp/UnOp dispatch, so a Kirito `a + b` and a
    // C++ `a + b` produce byte-identical values (same wraparound, same true-division, same throw-on-
    // zero-divisor, same per-type dispatch). Definitions below (after applyBinaryOp is declared).
    Value operator+(const Value& r) const;
    Value operator-(const Value& r) const;
    Value operator*(const Value& r) const;
    Value operator/(const Value& r) const;
    Value operator%(const Value& r) const;
    Value operator-() const;                                     // unary negation
    // Floor-div (`//`) and power (`**`) don't have C++ operators; expose them as named members.
    Value floordiv(const Value& r) const;
    Value pow(const Value& r) const;
    // Comparison — `==`/`!=` are exact structural equality (never throws on type mismatch);
    // `< <= > >=` follow the object protocol's ordering (throws for unordered pairs).
    bool operator==(const Value& r) const;
    bool operator!=(const Value& r) const;
    bool operator<(const Value& r) const;
    bool operator<=(const Value& r) const;
    bool operator>(const Value& r) const;
    bool operator>=(const Value& r) const;
    // Truthiness — `if (v)` mirrors Kirito's `if v:`; `!v` mirrors `not v`.
    explicit operator bool() const { return truthy(); }
    bool operator!() const { return !truthy(); }
    // Kirito's `in` / `not in`.
    bool contains(const Value& v) const { return ref().contains(*vm_, v.h_); }

    // Hash. Throws (`unhashable type '<T>'`) on an unhashable object — same policy as Set/Dict
    // insertion. Value-protocol identity is preserved: two `equals`-equal values hash the same.
    std::size_t hash() const {
        const Object& o = ref();
        if (!o.hashable()) throw KiritoError("unhashable type '" + o.typeName() + "'");
        return o.hash();
    }

    // Compatibility helpers — delegate to the appropriate typed wrapper. Definitions after Dict.
    std::vector<std::pair<Value, Value>> pairs() const;
    Value at(std::ptrdiff_t i) const;
    bool has(std::string_view k) const;
    Value get(std::string_view k) const;
    Value get(std::string_view k, Value dflt) const;

    // Call this Value as a callable, forwarding positional handles. The result is a fresh, not-yet-
    // rooted allocation, so pin it (adopting) — a GC before first use would otherwise sweep it. A19-2.
    Value call(std::span<const Handle> args) const {
        return Value::adopting(*vm_, ref().call(*vm_, args));
    }
    // Convenience: call with primitives / Values captured in an initializer list.
    Value call(std::initializer_list<detail::Anything> args) const {
        // Root each freshly-materialised arg handle: a later toHandle() alloc, or the callee's
        // newScope prologue, would otherwise sweep an earlier unrooted arg (A09-3).
        RootScope rs(*vm_);
        std::vector<Handle> hs;
        hs.reserve(args.size());
        for (const auto& a : args) hs.push_back(rs.add(a.toHandle(*vm_)));
        return call(std::span<const Handle>(hs));
    }

    // Attribute access, matching the object protocol. getAttr may synthesise a fresh value (a bound
    // method, a computed property), so pin the result against a GC before first use. A19-2.
    Value getAttr(std::string_view name) const {
        return Value::adopting(*vm_, ref().getAttr(*vm_, h_, name));
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
    // Adopt an allocation: store its handle AND pin it so a subsequent GC can't sweep it before the
    // user finishes building. Used by every fresh-alloc constructor (List(vm, {…}), Dict(vm, {…}),
    // Value(vm, "hi"), …). Copies of the wrapper share the pin via shared_ptr.
    void adopt(KiritoVM& vm, Handle fresh) {
        vm_ = &vm;
        h_ = fresh;
        pin_ = std::make_shared<detail::Pin>(vm, fresh);
    }
    KiritoVM* vm_ = nullptr;
    Handle h_{};
    std::shared_ptr<detail::Pin> pin_;      // GC pin for fresh-alloc wrappers; null when wrapping.
};

// ================================================================================================
// Bool — Kirito's boolean. Construct with `Bool(vm, true)` or the ambient `Value(vm, true)`; read
// the raw boolean with `.value()`.
// ================================================================================================
class Bool : public Value {
public:
    Bool() = default;
    Bool(KiritoVM& vm, Handle h, const char* who = "Bool") {
        vm_ = &vm; h_ = h;
        if (!isBool()) typeError(who, "Bool");
    }
    // Bool singletons don't allocate — no pin needed.
    explicit Bool(KiritoVM& vm, bool v) { vm_ = &vm; h_ = vm.makeBool(v); }

    bool value() const { return static_cast<const BoolVal&>(ref()).value(); }
    // EXPLICIT, matching Value's truthiness operator: `if (b)` / `!b` / `b && x` / `static_cast<bool>(b)`
    // still work (contextual conversions permit an explicit operator), but a Bool no longer silently
    // decays to `bool` — and thence to `int` — in arithmetic/indexing (`b + 1`, `arr[b]`, `b << 2`).
    // Use `.value()` for a raw `bool`. (An implicit operator here would reintroduce exactly the
    // bool→int footgun that Value's `explicit operator bool` was made explicit to avoid.)
    explicit operator bool() const { return value(); }
};

// ================================================================================================
// Integer — 64-bit signed integer. Includes `.compare(other, rel_tol, abs_tol)` (same predicate as
// Kirito's `n.compare(m, rel_tol, abs_tol)`), so tolerance-aware comparisons work directly on the
// wrapper without going through the object protocol.
// ================================================================================================
class Integer : public Value {
public:
    Integer() = default;
    Integer(KiritoVM& vm, Handle h, const char* who = "Integer") {
        vm_ = &vm; h_ = h;
        if (!isInt()) typeError(who, "Integer");
    }
    // Fresh Integer from any built-in signed integral type — pinned in adopt() against a mid-
    // expression GC.
    explicit Integer(KiritoVM& vm, int v)                 { adopt(vm, vm.makeInt(v)); }
    explicit Integer(KiritoVM& vm, long v)                { adopt(vm, vm.makeInt(v)); }
    explicit Integer(KiritoVM& vm, long long v)           { adopt(vm, vm.makeInt(v)); }
    explicit Integer(KiritoVM& vm, unsigned v)            { adopt(vm, vm.makeInt(static_cast<int64_t>(v))); }
    explicit Integer(KiritoVM& vm, unsigned long v)       { adopt(vm, vm.makeInt(static_cast<int64_t>(v))); }
    explicit Integer(KiritoVM& vm, unsigned long long v)  { adopt(vm, vm.makeInt(static_cast<int64_t>(v))); }

    // Raw int64.
    int64_t value() const { return static_cast<const IntVal&>(ref()).value(); }
    operator int64_t() const { return value(); }

    // Kirito's `.compare(other, rel_tol=1e-9, abs_tol=0)`: True iff |a - b| <= max(rel_tol*max(|a|,|b|), abs_tol).
    // Accepts any Integer or Float wrapper for `other`. Numeric `.compare` is intentionally tolerant —
    // `==` stays bit-exact IEEE-754 (see CLAUDE.md, "The boundary rule").
    bool compare(const Value& other, double rel_tol = 1e-9, double abs_tol = 0.0) const {
        return floatClose(static_cast<double>(value()), other.asFloat("compare"),
                          rel_tol, abs_tol);
    }
    bool compare(double other, double rel_tol = 1e-9, double abs_tol = 0.0) const {
        return floatClose(static_cast<double>(value()), other, rel_tol, abs_tol);
    }
    bool compare(int64_t other, double rel_tol = 1e-9, double abs_tol = 0.0) const {
        return floatClose(static_cast<double>(value()), static_cast<double>(other),
                          rel_tol, abs_tol);
    }
};

// ================================================================================================
// Float — IEEE-754 double. Same `.compare(other, rel_tol, abs_tol)` as Integer.
// ================================================================================================
class Float : public Value {
public:
    Float() = default;
    Float(KiritoVM& vm, Handle h, const char* who = "Float") {
        vm_ = &vm; h_ = h;
        if (!isFloat()) typeError(who, "Float");
    }
    explicit Float(KiritoVM& vm, double v)  { adopt(vm, vm.makeFloat(v)); }
    explicit Float(KiritoVM& vm, float v)   { adopt(vm, vm.makeFloat(static_cast<double>(v))); }

    double value() const { return static_cast<const FloatVal&>(ref()).value(); }
    operator double() const { return value(); }

    // Kirito's `.compare(other, rel_tol=1e-9, abs_tol=0)`.
    bool compare(const Value& other, double rel_tol = 1e-9, double abs_tol = 0.0) const {
        return floatClose(value(), other.asFloat("compare"), rel_tol, abs_tol);
    }
    bool compare(double other, double rel_tol = 1e-9, double abs_tol = 0.0) const {
        return floatClose(value(), other, rel_tol, abs_tol);
    }
    bool compare(int64_t other, double rel_tol = 1e-9, double abs_tol = 0.0) const {
        return floatClose(value(), static_cast<double>(other), rel_tol, abs_tol);
    }
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
    // Fresh String from raw UTF-8 — pinned so a later allocation can't sweep it.
    explicit String(KiritoVM& vm, std::string_view utf8) {
        adopt(vm, vm.makeString(std::string(utf8)));
    }
    explicit String(KiritoVM& vm, const char* utf8) {
        adopt(vm, vm.makeString(std::string(utf8)));
    }
    explicit String(KiritoVM& vm, std::string utf8) {
        adopt(vm, vm.makeString(std::move(utf8)));
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

    // Concatenation of two Strings — fast path bypassing the object protocol. `str + Value` and
    // other mixed cases fall through to `Value::operator+`.
    String operator+(const String& rhs) const { return String(*vm_, utf8() + rhs.utf8()); }
    using Value::operator+;                              // keep base overloads visible

    // Byte-exact equality with a raw literal — handy in tests (`s == "hi"`).
    bool operator==(std::string_view rhs) const { return utf8() == rhs; }
    bool operator==(const char* rhs) const { return utf8() == std::string_view(rhs); }
    bool operator!=(std::string_view rhs) const { return !(*this == rhs); }
    bool operator!=(const char* rhs) const { return !(*this == rhs); }
    using Value::operator==;                             // keep base overloads visible
    using Value::operator!=;
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
    // Fresh empty List — pinned so a later `.push(Value(vm, ...))` can't sweep us.
    explicit List(KiritoVM& vm) {
        adopt(vm, vm.alloc(std::make_unique<ListVal>()));
    }
    // Fresh List from an initializer list of any-type items.
    List(KiritoVM& vm, std::initializer_list<detail::Anything> items) {
        RootScope roots(vm);
        auto lv = std::make_unique<ListVal>();
        lv->elems.reserve(items.size());
        for (const auto& a : items) { Handle h = a.toHandle(vm); roots.add(h); lv->elems.push_back(h); }
        adopt(vm, vm.alloc(std::move(lv)));
    }
    // Fresh List from an existing vector of Handles (bulk).
    List(KiritoVM& vm, const std::vector<Handle>& handles) {
        RootScope roots(vm);  // the final vm.alloc may collect; root the not-yet-arena'd elements
        for (Handle h : handles) roots.add(h);
        auto lv = std::make_unique<ListVal>();
        lv->elems = handles;
        adopt(vm, vm.alloc(std::move(lv)));
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
        auto& l = mut();
        std::ptrdiff_t n = static_cast<std::ptrdiff_t>(l.elems.size());
        std::ptrdiff_t k = i < 0 ? i + n : i;
        if (k < 0 || k >= n) throw KiritoError("List index out of range");
        l.setElem(vm_->arena(), static_cast<std::size_t>(k), v.handle());  // barriered element write
    }

    // Append. Chainable for fluent construction. (Barriered: the wrapper API sees the write barrier
    // transparently, so an embedder pushing a young value into a promoted list Just Works.)
    List& push(const Value& v) { mut().append(vm_->arena(), v.handle()); return *this; }
    List& push(Handle h) { mut().append(vm_->arena(), h); return *this; }
    template <class T> List& push(T x) { return push(Value(*vm_, x)); }

    // Pop and return the last element. Popping removes the list's reference to it, so if nothing
    // else roots it a GC before the caller uses the result would sweep it — pin it (adopting). A19-2.
    Value pop() {
        auto& e = mut().elems;
        if (e.empty()) throw KiritoError("pop from empty list");
        Handle h = e.back();
        e.pop_back();
        return Value::adopting(*vm_, h);
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
        adopt(vm, vm.alloc(std::make_unique<DictVal>()));
    }
    Dict(KiritoVM& vm, std::initializer_list<
             std::pair<detail::Anything, detail::Anything>> entries) {
        RootScope roots(vm);
        auto dv = std::make_unique<DictVal>();
        for (const auto& [ka, va] : entries) {
            Handle kh = ka.toHandle(vm), vh = va.toHandle(vm);
            roots.add(kh); roots.add(vh);
            dv->set(vm.arena(), kh, vh);
        }
        adopt(vm, vm.alloc(std::move(dv)));
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
    // Overloads accepting a raw Handle key.
    Dict& set(Handle k, Handle v) { return set(Value(*vm_, k), Value(*vm_, v)); }
    Dict& set(Handle k, const Value& v) { return set(Value(*vm_, k), v); }

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
        adopt(vm, vm.alloc(std::make_unique<SetVal>()));
    }
    Set(KiritoVM& vm, std::initializer_list<detail::Anything> items) {
        RootScope roots(vm);
        auto sv = std::make_unique<SetVal>();
        for (const auto& a : items) { Handle h = a.toHandle(vm); roots.add(h); sv->add(vm.arena(), h); }
        adopt(vm, vm.alloc(std::move(sv)));
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

    // Enforce a minimum arity. Wording matches the low-layer `requireArgs` (native.hpp) exactly, so
    // arity diagnostics are uniform interpreter-wide, and follows the existing house style for a
    // "not enough" error ("expected at least N values to unpack, got M") — informative about both
    // the requirement and what was actually passed.
    void require(std::size_t n) const {
        if (a_.size() < n)
            throw KiritoError(std::string(fn_) + "() expected at least " + std::to_string(n) +
                              " argument(s), got " + std::to_string(a_.size()));
    }
private:
    KiritoVM* vm_;
    std::span<const Handle> a_;
    const char* fn_;
};

// ================================================================================================
// PinnedHandle — an owning GC root you can STORE. A host that keeps a Kirito value in a long-lived
// C++ object (a class member, a `std::vector`, a callback registry) can't use `RootScope` (it is
// stack-scoped) and must not hold a bare `Handle` (the collector can't see it, so a mid-run GC
// sweeps it — a dangling handle). `PinnedHandle` pins its handle for its own lifetime via
// `KiritoVM::pinHandle`/`unpinHandle`, so the object survives every collection until the pin is
// destroyed. It is copyable (each copy holds its own refcounted pin) and movable (the pin transfers),
// converts to `Handle` for the raw protocol, and yields a `Value` wrapper on demand.
//
//   class Engine {
//     KiritoVM& vm_;
//     PinnedHandle policy_;              // a compiled Function — safe to keep across many calls
//   public:
//     Engine(KiritoVM& vm, Handle policy) : vm_(vm), policy_(vm, policy) {}
//     Value run(Value arg) { return policy_.value().call({arg}); }
//   };
// ================================================================================================
class PinnedHandle {
public:
    PinnedHandle() = default;
    PinnedHandle(KiritoVM& vm, Handle h) : vm_(&vm), h_(h) { vm_->pinHandle(h_); }
    explicit PinnedHandle(const Value& v) : PinnedHandle(v.vm(), v.handle()) {}

    ~PinnedHandle() { reset(); }

    // Copy: each PinnedHandle owns an independent pin (pinHandle is refcounted).
    PinnedHandle(const PinnedHandle& o) : vm_(o.vm_), h_(o.h_) { if (vm_) vm_->pinHandle(h_); }
    PinnedHandle& operator=(const PinnedHandle& o) {
        if (this != &o) {
            if (o.vm_) o.vm_->pinHandle(o.h_);      // pin the new before releasing the old (self-safe)
            reset();
            vm_ = o.vm_;
            h_ = o.h_;
        }
        return *this;
    }
    // Move: steal the pin, leave the source empty.
    PinnedHandle(PinnedHandle&& o) noexcept : vm_(o.vm_), h_(o.h_) { o.vm_ = nullptr; }
    PinnedHandle& operator=(PinnedHandle&& o) noexcept {
        if (this != &o) { reset(); vm_ = o.vm_; h_ = o.h_; o.vm_ = nullptr; }
        return *this;
    }

    // Release the pin now (idempotent); leaves the PinnedHandle empty.
    void reset() {
        if (vm_) { vm_->unpinHandle(h_); vm_ = nullptr; }
    }

    bool pinned() const { return vm_ != nullptr; }
    Handle handle() const { return h_; }
    operator Handle() const { return h_; }
    KiritoVM& vm() const { return *vm_; }
    // Wrap for use with the ergonomic API. The returned Value is a plain view (not itself pinned) —
    // this PinnedHandle is what keeps the object alive.
    Value value() const { return Value(*vm_, h_); }

private:
    KiritoVM* vm_ = nullptr;
    Handle    h_{};
};

// ================================================================================================
// Deferred definitions — the typed wrappers, `Anything::Anything(const Value&)`, and the Bytes
// helpers that depend on the BytesVal layout.
// ================================================================================================

inline detail::Anything::Anything(const Value& val) : v(val.handle()) {}

inline Bool    Value::asBoolV(const char* who)   const { return Bool(*vm_, h_, who); }
inline Integer Value::asInteger(const char* who) const { return Integer(*vm_, h_, who); }
inline Float   Value::asFloatV(const char* who)  const { return Float(*vm_, h_, who); }
inline String  Value::asString(const char* who)  const { return String(*vm_, h_, who); }
inline List    Value::asList(const char* who)    const { return List(*vm_, h_, who); }
inline Dict    Value::asDict(const char* who)    const { return Dict(*vm_, h_, who); }
inline Set     Value::asSet(const char* who)     const { return Set(*vm_, h_, who); }
inline Bytes   Value::asBytes(const char* who)   const { return Bytes(*vm_, h_, who); }

inline std::optional<Bool> Value::tryBoolV() const {
    return isBool() ? std::optional<Bool>(Bool(*vm_, h_)) : std::nullopt;
}
inline std::optional<Integer> Value::tryInteger() const {
    return isInt() ? std::optional<Integer>(Integer(*vm_, h_)) : std::nullopt;
}
inline std::optional<Float> Value::tryFloatV() const {
    return isFloat() ? std::optional<Float>(Float(*vm_, h_)) : std::nullopt;
}
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
// getItem may allocate a fresh result (a 1-char String, a Bytes byte, a slice), so pin it against a
// GC before first use — a bare String/Bytes index otherwise returns a dangling handle. A19-2.
inline Value Value::at(std::ptrdiff_t i) const {
    Handle key = vm_->makeInt(static_cast<int64_t>(i));
    std::array<Handle, 1> keys{key};
    return Value::adopting(*vm_, ref().getItem(*vm_, keys));
}
inline std::vector<std::pair<Value, Value>> Value::pairs() const { return asDict("pairs").pairs(); }
inline bool Value::has(std::string_view k) const { return asDict("has").contains(k); }
inline Value Value::get(std::string_view k) const { return asDict("get").at(Value(*vm_, k)); }
inline Value Value::get(std::string_view k, Value dflt) const { return asDict("get").get(k, dflt); }

// --- Operator definitions — delegate to applyBinaryOp/applyUnaryOp (defined in runtime.hpp) -------
// Each arithmetic/unary op allocates a fresh result (a Float, a big Integer, a String/List concat)
// that is not yet rooted anywhere the GC can see, so it must be PINNED (adopting) — else a GC
// between the op and the caller's first use of the result sweeps it (dangling handle / UAF). A19-1.
inline Value Value::operator+(const Value& r) const {
    return Value::adopting(*vm_, applyBinaryOp(*vm_, BinOp::Add, h_, r.h_));
}
inline Value Value::operator-(const Value& r) const {
    return Value::adopting(*vm_, applyBinaryOp(*vm_, BinOp::Sub, h_, r.h_));
}
inline Value Value::operator*(const Value& r) const {
    return Value::adopting(*vm_, applyBinaryOp(*vm_, BinOp::Mul, h_, r.h_));
}
inline Value Value::operator/(const Value& r) const {
    return Value::adopting(*vm_, applyBinaryOp(*vm_, BinOp::Div, h_, r.h_));
}
inline Value Value::operator%(const Value& r) const {
    return Value::adopting(*vm_, applyBinaryOp(*vm_, BinOp::Mod, h_, r.h_));
}
inline Value Value::operator-() const {
    return Value::adopting(*vm_, applyUnaryOp(*vm_, UnOp::Neg, h_));
}
inline Value Value::floordiv(const Value& r) const {
    return Value::adopting(*vm_, applyBinaryOp(*vm_, BinOp::FloorDiv, h_, r.h_));
}
inline Value Value::pow(const Value& r) const {
    return Value::adopting(*vm_, applyBinaryOp(*vm_, BinOp::Pow, h_, r.h_));
}
inline bool Value::operator==(const Value& r) const {
    Handle h = applyBinaryOp(*vm_, BinOp::Eq, h_, r.h_);
    return vm_->arena().deref(h).truthy();  // truthy(), not static_cast<BoolVal&>: a user _eq_/_lt_/…
                                            // may return a non-Bool (applyBinaryOp hands it back raw),
                                            // which the cast would type-confuse (UB). truthy() matches
                                            // how Kirito's own `if a == b` consumes the result.
}
inline bool Value::operator!=(const Value& r) const {
    Handle h = applyBinaryOp(*vm_, BinOp::Ne, h_, r.h_);
    return vm_->arena().deref(h).truthy();  // truthy(), not static_cast<BoolVal&>: a user _eq_/_lt_/…
                                            // may return a non-Bool (applyBinaryOp hands it back raw),
                                            // which the cast would type-confuse (UB). truthy() matches
                                            // how Kirito's own `if a == b` consumes the result.
}
inline bool Value::operator<(const Value& r) const {
    Handle h = applyBinaryOp(*vm_, BinOp::Lt, h_, r.h_);
    return vm_->arena().deref(h).truthy();  // truthy(), not static_cast<BoolVal&>: a user _eq_/_lt_/…
                                            // may return a non-Bool (applyBinaryOp hands it back raw),
                                            // which the cast would type-confuse (UB). truthy() matches
                                            // how Kirito's own `if a == b` consumes the result.
}
inline bool Value::operator<=(const Value& r) const {
    Handle h = applyBinaryOp(*vm_, BinOp::Le, h_, r.h_);
    return vm_->arena().deref(h).truthy();  // truthy(), not static_cast<BoolVal&>: a user _eq_/_lt_/…
                                            // may return a non-Bool (applyBinaryOp hands it back raw),
                                            // which the cast would type-confuse (UB). truthy() matches
                                            // how Kirito's own `if a == b` consumes the result.
}
inline bool Value::operator>(const Value& r) const {
    Handle h = applyBinaryOp(*vm_, BinOp::Gt, h_, r.h_);
    return vm_->arena().deref(h).truthy();  // truthy(), not static_cast<BoolVal&>: a user _eq_/_lt_/…
                                            // may return a non-Bool (applyBinaryOp hands it back raw),
                                            // which the cast would type-confuse (UB). truthy() matches
                                            // how Kirito's own `if a == b` consumes the result.
}
inline bool Value::operator>=(const Value& r) const {
    Handle h = applyBinaryOp(*vm_, BinOp::Ge, h_, r.h_);
    return vm_->arena().deref(h).truthy();  // truthy(), not static_cast<BoolVal&>: a user _eq_/_lt_/…
                                            // may return a non-Bool (applyBinaryOp hands it back raw),
                                            // which the cast would type-confuse (UB). truthy() matches
                                            // how Kirito's own `if a == b` consumes the result.
}

}  // namespace kirito

#endif
