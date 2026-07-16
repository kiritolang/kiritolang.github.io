#ifndef KIRITO_RUNTIME_HPP
#define KIRITO_RUNTIME_HPP

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "ast.hpp"
#include "builtins.hpp"
#include "class_value.hpp"
#include "collections.hpp"
#include "function.hpp"
#include "lexer.hpp"
#include "module.hpp"
#include "native.hpp"
#include "object.hpp"
#include "parser.hpp"
#include "stdlib_io.hpp"
#include "stdlib_path.hpp"
#include "stdlib_math.hpp"
#include "stdlib_random.hpp"
#include "stdlib_matrix.hpp"
#include "stdlib_json.hpp"
#include "stdlib_net.hpp"
#include "stdlib_serialize.hpp"
#include "stdlib_sys.hpp"
#include "stdlib_time.hpp"
#include "stdlib_dump.hpp"
#include "stdlib_zlib.hpp"
#include "stdlib_hash.hpp"
#include "stdlib_crypto.hpp"
#include "stdlib_int.hpp"
#include "stdlib_regex.hpp"
#include "stdlib_kimodules.hpp"
#include "vm.hpp"

// Definitions that need a complete KiritoVM (and the front end): they live here, included last,
// so the per-component headers stay free of upward dependencies.

namespace kirito {

// The native-binding idiom below re-uses `vm`/`self` as bound-method lambda parameters that
// intentionally shadow the enclosing getAttr/setup `vm`/`self` (same VM, by design). Silence
// -Wshadow for these mechanical bindings; it stays active in the compiler/VM/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

// --- generational write barrier (declared in object.hpp) ------------------------------------
// Record an old->young edge so a minor GC will not free a still-reachable young object. Placed
// inside the core value mutators, so it fires for both the interpreter and the C++ API. Hot path:
// a YOUNG container early-outs on one byte test (fresh call scopes, freshly-built containers). Only
// an OLD container that gains a YOUNG value enrols in the remembered set — and only once per cycle.
inline void gcWriteBarrier(ObjectArena& arena, Object* container, Handle value) {
    if (container->gcYoung() || container->gcRemembered()) return;
    if (value.generation == 0) return;                  // null/sentinel handle: points to nothing
    if (arena.deref(value).gcYoung()) arena.remember(container);
}
// (There is deliberately NO arena-less overload. One existed, resolving the arena via activeVM() —
// the most recently CONSTRUCTED live VM on this thread, which is not necessarily the one that owns
// `container`. With two VMs on one thread — which the embedding API explicitly allows — it would ask
// VM_B about a handle from VM_A: a spurious "dangling handle"/"stale generation" throw from a plain
// setAttr, or, when the slot and generation happened to collide, a silently wrong young/old answer
// that skipped remember() and let VM_A's next minor free a still-reachable value. Every mutator now
// takes the owning arena from its caller, who alone knows it. v1.15 A05-1.)

// kMaxRepeat (the ~256 MB repetition/padding cap) is defined in common.hpp so bytes.hpp shares it.

// Forward decl (defined near installBuiltins): fully iterate `src`, rooting every element in `rs` so
// an allocating callback/rehash mid-loop can't sweep an unconsumed element. Used by both the built-in
// type methods (below) and the free builtins, so the rooting is single-sourced.
inline std::vector<Handle> rootedIterate(KiritoVM& vm, Handle src, RootScope& rs, const char* err);

// --- numeric helpers ------------------------------------------------------------------------

inline bool isNumeric(const Object& o) {
    return o.kind() == ValueKind::Integer || o.kind() == ValueKind::Float;
}
inline double asDouble(const Object& o) {
    return o.kind() == ValueKind::Integer
               ? static_cast<double>(static_cast<const IntVal&>(o).value())
               : static_cast<const FloatVal&>(o).value();
}
inline bool floatEqual(double l, double r) {
    // EXACT IEEE-754 equality: NaN != NaN (not even itself), inf == inf, 0.0 == -0.0.
    // `==`/`!=` thus agree with `<`/`>` (trichotomy holds) and with hashing, so distinct-but-close
    // floats no longer collide as Set/Dict keys. For "approximately equal", use the .compare(other,
    // rel_tol=, abs_tol=) method on Integer/Float.
    return l == r;
}
// (floatClose — the numeric .compare() tolerance — now lives in common.hpp so the stdlib modules,
// compiled before this header, can share the one implementation.)
// Exact three-way comparison of an int64 and a double, WITHOUT the lossy (double)int round-trip that
// collapses |int| > 2^53 (e.g. 2^53+1 and the float 2^53 are distinct, and INT64_MAX != 2.0^63).
// Returns -1 (i < f), 0 (i == f), +1 (i > f), or 2 (UNORDERED — f is NaN). This is what keeps
// Integer↔Float ==/!=/<,<=,>,>= EXACT, so equality agrees with ordering and with hashing.
inline int compareIntFloat(int64_t i, double f) {
    if (f != f) return 2;                                 // NaN is unordered with everything
    constexpr double kTwo63 = 9223372036854775808.0;      // 2^63, exact in double (-kTwo63 == INT64_MIN)
    if (f >= kTwo63) return -1;                            // f >= 2^63 > every int64  -> i < f
    if (f < -kTwo63) return +1;                            // f < -2^63 < every int64  -> i > f
    double ff = std::floor(f);                             // f now in [-2^63, 2^63): floor(f) fits int64
    int64_t fi = static_cast<int64_t>(ff);                // exact (ff integral and in range)
    if (i < fi) return -1;
    if (i > fi) return +1;
    return f > ff ? -1 : 0;                                // i == floor(f); a fractional part means i < f
}
inline bool intFloatEqual(int64_t i, double f) { return compareIntFloat(i, f) == 0; }
// Two's-complement wraparound for the int64 operators. Signed overflow is UB in C++, so we do the
// arithmetic in uint64_t (where wraparound is defined) and reinterpret — giving consistent,
// well-defined behavior on overflow instead of undefined behavior. (Kirito ints are fixed int64;
// arbitrary-precision integers are a future enrichment.)
inline int64_t wadd(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}
inline int64_t wsub(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
}
inline int64_t wmul(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}
inline int64_t ifloordiv(int64_t a, int64_t b) {
    if (b == -1) return wsub(0, a);  // INT64_MIN / -1 would overflow; wrap instead of UB
    int64_t q = a / b, r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) --q;
    return q;
}
inline int64_t imod(int64_t a, int64_t b) {
    if (b == -1) return 0;           // a % -1 is always 0 (avoids the INT64_MIN/-1 UB)
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}
inline int64_t ipow(int64_t base, int64_t exp) {
    int64_t result = 1;
    while (exp > 0) {
        if (exp & 1) result = wmul(result, base);
        exp >>= 1;
        if (exp) base = wmul(base, base);
    }
    return result;
}

// Exact ordering of two numeric Objects (Integer/Float in any mix): -1/0/+1, or 2 for UNORDERED (a
// NaN operand). Integer↔Integer and Integer↔Float compare without precision loss (see compareIntFloat);
// only Float↔Float uses the native double order. Shared by numericBinary's `<,<=,>,>=` and kiLessThan.
inline int numericCompare(const Object& a, const Object& b) {
    bool ai = a.kind() == ValueKind::Integer, bi = b.kind() == ValueKind::Integer;
    if (ai && bi) {
        int64_t x = static_cast<const IntVal&>(a).value(), y = static_cast<const IntVal&>(b).value();
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (ai) return compareIntFloat(static_cast<const IntVal&>(a).value(), static_cast<const FloatVal&>(b).value());
    if (bi) {
        int c = compareIntFloat(static_cast<const IntVal&>(b).value(), static_cast<const FloatVal&>(a).value());
        return c == 2 ? 2 : -c;                            // flip the sense; UNORDERED stays UNORDERED
    }
    double x = static_cast<const FloatVal&>(a).value(), y = static_cast<const FloatVal&>(b).value();
    if (x != x || y != y) return 2;                        // either NaN -> unordered
    return x < y ? -1 : (x > y ? 1 : 0);
}

// Shared arithmetic dispatch for Integer/Float. Integer⊕Integer stays Integer (except `/`),
// any Float promotes to Float, and `/` always yields Float.
inline Handle numericBinary(KiritoVM& vm, BinOp op, Handle aH, Handle bH) {
    const Object& a = vm.arena().deref(aH);
    const Object& b = vm.arena().deref(bH);
    // Both operands must be numeric. The operator path only reaches here via a numeric left
    // operand's binary(), but builtins (pow / round / divmod) call this directly with raw args, so
    // a non-numeric `a` is possible — guard it, otherwise asDouble() would downcast it to FloatVal
    // (type-confusion UB) instead of throwing a clean error.
    if (!isNumeric(a) || !isNumeric(b)) {
        bool cmp = op == BinOp::Lt || op == BinOp::Le || op == BinOp::Gt || op == BinOp::Ge;
        const Object& bad = !isNumeric(a) ? a : b;
        const Object& other = !isNumeric(a) ? b : a;
        throw KiritoError("unsupported operand type '" + bad.typeName() + "' for " +
                          (cmp ? "comparison" : "arithmetic") + " with '" + other.typeName() + "'");
    }

    if (op == BinOp::Div) {
        double db = asDouble(b);
        if (db == 0.0) throw KiritoError("division by zero");
        return vm.makeFloat(asDouble(a) / db);
    }

    // Integer-vs-Integer is the common hot case: compare/arith directly in int64 (no double round-
    // trip, so large magnitudes compare exactly and it's faster).
    if (a.kind() == ValueKind::Integer && b.kind() == ValueKind::Integer) {
        int64_t x = static_cast<const IntVal&>(a).value();
        int64_t y = static_cast<const IntVal&>(b).value();
        switch (op) {
            case BinOp::Lt: { return vm.makeBool(x < y); } break;
            case BinOp::Le: { return vm.makeBool(x <= y); } break;
            case BinOp::Gt: { return vm.makeBool(x > y); } break;
            case BinOp::Ge: { return vm.makeBool(x >= y); } break;
            case BinOp::Add: { return vm.makeInt(wadd(x, y)); } break;
            case BinOp::Sub: { return vm.makeInt(wsub(x, y)); } break;
            case BinOp::Mul: { return vm.makeInt(wmul(x, y)); } break;
            case BinOp::FloorDiv: {
                if (y == 0) throw KiritoError("integer division by zero");
                return vm.makeInt(ifloordiv(x, y));
            } break;
            case BinOp::Mod: {
                if (y == 0) throw KiritoError("integer modulo by zero");
                return vm.makeInt(imod(x, y));
            } break;
            case BinOp::Pow: {
                if (y < 0) {
                    if (x == 0) throw KiritoError("zero cannot be raised to a negative power");
                    return vm.makeFloat(std::pow(static_cast<double>(x), static_cast<double>(y)));
                }
                return vm.makeInt(ipow(x, y));
            } break;
            default: { } break;
        }
    }

    // Mixed Integer/Float ordering: compare EXACTLY (a NaN operand -> all four false, IEEE-correct).
    if (op == BinOp::Lt || op == BinOp::Le || op == BinOp::Gt || op == BinOp::Ge) {
        int c = numericCompare(a, b);
        switch (op) {
            case BinOp::Lt: return vm.makeBool(c == -1);
            case BinOp::Le: return vm.makeBool(c == -1 || c == 0);
            case BinOp::Gt: return vm.makeBool(c == 1);
            case BinOp::Ge: return vm.makeBool(c == 1 || c == 0);
            default: break;
        }
    }

    {
        // At least one operand is a Float (Integer×Integer was handled above): promote and compute.
        double x = asDouble(a), y = asDouble(b);
        switch (op) {
            case BinOp::Add: { return vm.makeFloat(x + y); } break;
            case BinOp::Sub: { return vm.makeFloat(x - y); } break;
            case BinOp::Mul: { return vm.makeFloat(x * y); } break;
            case BinOp::FloorDiv: {
                if (y == 0.0) throw KiritoError("float division by zero");
                return vm.makeFloat(std::floor(x / y));
            } break;
            case BinOp::Mod: {
                if (y == 0.0) throw KiritoError("float modulo by zero");
                return vm.makeFloat(x - std::floor(x / y) * y);
            } break;
            case BinOp::Pow: {
                // Match math.pow's domain policy (a NaN operand passes through): 0**-n (= 1/0) and a
                // negative base to a fractional power both RAISE instead of silently yielding inf/NaN.
                if (!std::isnan(x) && !std::isnan(y)) {
                    if (x == 0.0 && y < 0.0) throw KiritoError("zero cannot be raised to a negative power");
                    if (x < 0.0 && y != std::floor(y)) throw KiritoError("a negative base cannot be raised to a fractional power");
                }
                return vm.makeFloat(std::pow(x, y));
            } break;
            default: { } break;
        }
    }
    throw KiritoError("unsupported numeric operator");
}

// --- IntVal / FloatVal out-of-line members ---------------------------------------------------

inline bool IntVal::equals(const ObjectArena&, const Object& other) const {
    if (other.kind() == ValueKind::Integer) return value_ == static_cast<const IntVal&>(other).value();
    if (other.kind() == ValueKind::Float)  // EXACT (no lossy (double)int): 2^53+1 != the float 2^53
        return intFloatEqual(value_, static_cast<const FloatVal&>(other).value());
    return false;
}
inline Handle IntVal::binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) {
    // `Integer * seq` is sequence repetition (either order is allowed) — defer to the sequence,
    // whose `*` already takes an Integer count. Covers List/Array/String/Bytes (3 * "ab" == "ababab").
    if (op == BinOp::Mul) {
        Object& r = vm.arena().deref(rhs);
        ValueKind rk = r.kind();
        if (rk == ValueKind::List || rk == ValueKind::Array || rk == ValueKind::String ||
            dynamic_cast<const BytesVal*>(&r))  // Bytes is a NativeClass (kind == Instance)
            return r.binary(vm, BinOp::Mul, rhs, self);
    }
    return numericBinary(vm, op, self, rhs);
}
inline Handle IntVal::unary(KiritoVM& vm, UnOp op, Handle) {
    if (op == UnOp::Neg) return vm.makeInt(wsub(0, value_));  // wrap (-INT64_MIN would be UB)
    throw KiritoError("Integer does not support this unary operator");
}

inline bool FloatVal::equals(const ObjectArena&, const Object& other) const {
    if (other.kind() == ValueKind::Float) return floatEqual(value_, static_cast<const FloatVal&>(other).value());
    if (other.kind() == ValueKind::Integer)  // EXACT (no lossy (double)int), symmetric with IntVal::equals
        return intFloatEqual(static_cast<const IntVal&>(other).value(), value_);
    return false;
}
inline std::size_t FloatVal::hash() const {
    // An integral Float that denotes an exact int64 must hash identically to that Integer, so `==`
    // agrees with hashing (Set/Dict membership). The range must match intFloatEqual EXACTLY: [-2^63, 2^63)
    // (NaN/±inf fall through to the double hash — inf == floor(inf) but is out of range).
    if (value_ == std::floor(value_) &&
        value_ >= -9223372036854775808.0 && value_ < 9223372036854775808.0)
        return std::hash<int64_t>{}(static_cast<int64_t>(value_));
    return std::hash<double>{}(value_);
}
inline Handle FloatVal::binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) {
    return numericBinary(vm, op, self, rhs);
}
inline Handle FloatVal::unary(KiritoVM& vm, UnOp op, Handle) {
    if (op == UnOp::Neg) return vm.makeFloat(-value_);
    throw KiritoError("Float does not support this unary operator");
}

// `.compare(other, rel_tol=1e-9, abs_tol=0.0) -> Bool` — approximate equality (rel/abs tolerance)
// shared by Integer and Float, since `==` is now exact IEEE-754. The receiver is captured
// so the GC keeps it alive while the bound method exists; the signature gives keyword args + inspect.
inline Handle makeNumericCompare(KiritoVM& vm, Handle self) {
    RootScope rs(vm);
    return vm.alloc(std::make_unique<NativeFunction>(
        "compare", toleranceSig(vm, rs), "Bool",
        [self](KiritoVM& v, std::span<const Handle> a) -> Handle {
            const Object& other = v.arena().deref(a[0]);
            if (!isNumeric(other))
                throw KiritoError("compare() expects a number, not '" + other.typeName() + "'");
            const Object& rel = v.arena().deref(a[1]);
            const Object& abst = v.arena().deref(a[2]);
            if (!isNumeric(rel) || !isNumeric(abst))
                throw KiritoError("compare() rel_tol and abs_tol must be numbers");
            return v.makeBool(floatClose(asDouble(v.arena().deref(self)), asDouble(other),
                                         asDouble(rel), asDouble(abst)));
        },
        std::vector<Handle>{self}));
}
inline Handle IntVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    if (name == "compare") return makeNumericCompare(vm, self);
    return Object::getAttr(vm, self, name);
}
inline std::vector<std::string> IntVal::inspectMembers() const {
    return {"compare(other, rel_tol = 1e-09, abs_tol = 0.0) -> Bool"};
}
inline Handle FloatVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    if (name == "compare") return makeNumericCompare(vm, self);
    return Object::getAttr(vm, self, name);
}
inline std::vector<std::string> FloatVal::inspectMembers() const {
    return {"compare(other, rel_tol = 1e-09, abs_tol = 0.0) -> Bool"};
}

// --- StrVal out-of-line members --------------------------------------------------------------

inline Handle StrVal::binary(KiritoVM& vm, BinOp op, Handle, Handle rhs) {
    const Object& b = vm.arena().deref(rhs);
    if (op == BinOp::Add) {
        if (b.kind() != ValueKind::String)
            throw KiritoError("can only concatenate String to String, not '" + b.typeName() + "'");
        return vm.makeString(value_ + static_cast<const StrVal&>(b).value());
    }
    if (op == BinOp::Mul) {
        if (b.kind() != ValueKind::Integer)
            throw KiritoError("can only repeat String by an Integer");
        int64_t n = static_cast<const IntVal&>(b).value();
        if (n <= 0 || value_.empty()) return vm.makeString("");
        // Guard against absurd allocations from a dumb/hostile count (e.g. "x" * 10**12): reject up
        // front rather than OOM the host. The product is computed in unsigned to avoid overflow UB.
        if (static_cast<uint64_t>(n) > kMaxRepeat / value_.size())
            throw KiritoError("repeated String too large");
        std::string out;
        out.reserve(value_.size() * static_cast<std::size_t>(n));
        for (int64_t i = 0; i < n; ++i) out += value_;
        return vm.makeString(std::move(out));
    }
    if (b.kind() == ValueKind::String) {
        const std::string& r = static_cast<const StrVal&>(b).value();
        switch (op) {
            case BinOp::Lt: { return vm.makeBool(value_ < r); } break;
            case BinOp::Le: { return vm.makeBool(value_ <= r); } break;
            case BinOp::Gt: { return vm.makeBool(value_ > r); } break;
            case BinOp::Ge: { return vm.makeBool(value_ >= r); } break;
            default: { } break;
        }
    }
    throw KiritoError("type 'String' does not support this operator");
}

// --- Collection out-of-line members ----------------------------------------------------------

// Resolve an Integer key against a sequence length, supporting negative indices (count from the end).
inline std::size_t sequenceIndex(KiritoVM& vm, std::size_t size, Handle key) {
    const Object& k = vm.arena().deref(key);
    if (k.kind() != ValueKind::Integer)
        throw KiritoError("index must be Integer, not '" + k.typeName() + "'");
    int64_t i = static_cast<const IntVal&>(k).value();
    int64_t n = static_cast<int64_t>(size);
    if (i < 0) i += n;
    if (i < 0 || i >= n) throw KiritoError("index out of range");
    return static_cast<std::size_t>(i);
}

// Value equality that respects a user class's `_eq_` (defined below; forward-declared so the List
// methods above can use it for `index`/`count`/`remove`).
inline bool kiEquals(KiritoVM& vm, Handle a, Handle b);

// Ordering for sort()/comparisons: numbers numerically, Strings and Lists lexicographically, else a
// type error. List ordering compares element-by-element (recursively) and breaks ties by length —
// enabling the common multi-key sort idiom (key returns a [k1, k2, ...] list).
inline bool kiLessThan(KiritoVM& vm, Handle a, Handle b) {
    EqualsGuard guard;  // bound the element-wise recursion on nested lists (a cyclic/deep structure
                        // would otherwise overflow the native stack), exactly as kiEquals does for ==
    const Object& x = vm.arena().deref(a);
    const Object& y = vm.arena().deref(b);
    // Numbers compare EXACTLY (Integer↔Integer and Integer↔Float both avoid the lossy double round-trip
    // that would collapse int64 magnitudes beyond 2^53 and mis-order sort/sorted/min/max + List compares).
    if (isNumeric(x) && isNumeric(y)) {
        int c = numericCompare(x, y);
        if (c != 2) return c == -1;
        // A NaN operand makes numericCompare "unordered". Returning false both ways would make NaN
        // equivalent to EVERY number (NaN~1, NaN~2 but 1<2) — not a strict weak ordering, so
        // std::sort / min / max are undefined. Impose a total order: NaN sorts as the largest value
        // (after every real, incl. +inf) and NaN is not less than NaN. So x<y iff only y is NaN.
        auto isNan = [](const Object& o) { return o.kind() == ValueKind::Float &&
                                                  static_cast<const FloatVal&>(o).value() != static_cast<const FloatVal&>(o).value(); };
        return isNan(y) && !isNan(x);
    }
    if (x.kind() == ValueKind::String && y.kind() == ValueKind::String)
        return static_cast<const StrVal&>(x).value() < static_cast<const StrVal&>(y).value();
    // Bytes order lexicographically by unsigned byte (as the `< <= > >=` operators already do) — so
    // sorted()/min()/max() work on Bytes too. std::string's `<` is char_traits<char> = unsigned-byte
    // memcmp order, exactly the byte order we want. (Bytes is a NativeClass, so kind() == Instance.)
    if (auto* xb = dynamic_cast<const BytesVal*>(&x))
        if (auto* yb = dynamic_cast<const BytesVal*>(&y))
            return xb->data < yb->data;
    if ((x.kind() == ValueKind::List || x.kind() == ValueKind::Array) &&
        (y.kind() == ValueKind::List || y.kind() == ValueKind::Array)) {
        const auto& xe = static_cast<const ListVal&>(x).elems;
        const auto& ye = static_cast<const ListVal&>(y).elems;
        std::size_t common = std::min(xe.size(), ye.size());
        for (std::size_t i = 0; i < common; ++i) {
            if (kiLessThan(vm, xe[i], ye[i])) return true;
            if (kiLessThan(vm, ye[i], xe[i])) return false;
        }
        return xe.size() < ye.size();  // shared prefix equal: shorter list is "less"
    }
    // A user class can define ordering via `_lt_`, so sorted()/min()/max() and `<` work on instances.
    if (x.kind() == ValueKind::Instance)
        if (auto* inst = dynamic_cast<const InstanceValue*>(&x); inst && inst->findMethod(vm.arena(), "_lt_"))
            return vm.arena().deref(vm.arena().deref(a).binary(vm, BinOp::Lt, a, b)).truthy();
    throw KiritoError("cannot order '" + x.typeName() + "' and '" + y.typeName() + "'");
}

inline Handle ListVal::getItem(KiritoVM& vm, std::span<const Handle> keys) {
    return elems[sequenceIndex(vm, elems.size(), singleKey(*this, keys))];
}
inline void ListVal::setItem(KiritoVM& vm, std::span<const Handle> keys, Handle value) {
    setElem(vm.arena(), sequenceIndex(vm, elems.size(), singleKey(*this, keys)), value);
}
inline Handle ListVal::binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) {
    const Object& b = vm.arena().deref(rhs);
    // `+` concatenates two Lists into a new List.
    if (op == BinOp::Add) {
        if (b.kind() != ValueKind::List && b.kind() != ValueKind::Array)
            throw KiritoError("can only concatenate List to List, not '" + b.typeName() + "'");
        RootScope rs(vm);
        auto out = std::make_unique<ListVal>();
        out->elems = elems;
        const auto& be = static_cast<const ListVal&>(b).elems;
        out->elems.insert(out->elems.end(), be.begin(), be.end());
        return vm.alloc(std::move(out));
    }
    // `*` repeats a List by an Integer count (element handles are shared, so
    // `[[0]] * 3` is three references to the same inner list). Guarded against absurd counts.
    if (op == BinOp::Mul) {
        if (b.kind() != ValueKind::Integer)
            throw KiritoError("can only repeat List by an Integer");
        int64_t n = static_cast<const IntVal&>(b).value();
        RootScope rs(vm);
        auto out = std::make_unique<ListVal>();
        if (n > 0 && !elems.empty()) {
            if (static_cast<uint64_t>(n) > kMaxRepeat / elems.size())
                throw KiritoError("repeated List too large");
            out->elems.reserve(elems.size() * static_cast<std::size_t>(n));
            for (int64_t i = 0; i < n; ++i)
                out->elems.insert(out->elems.end(), elems.begin(), elems.end());
        }
        return vm.alloc(std::move(out));
    }
    // Ordering: lexicographic, element-by-element (via kiLessThan), only against another List.
    if (op == BinOp::Lt || op == BinOp::Le || op == BinOp::Gt || op == BinOp::Ge) {
        if (b.kind() != ValueKind::List && b.kind() != ValueKind::Array)
            throw KiritoError("cannot order 'List' and '" + b.typeName() + "'");
        bool lt = kiLessThan(vm, self, rhs);
        bool gt = kiLessThan(vm, rhs, self);
        switch (op) {
            case BinOp::Lt: { return vm.makeBool(lt); } break;
            case BinOp::Le: { return vm.makeBool(!gt); } break;
            case BinOp::Gt: { return vm.makeBool(gt); } break;
            case BinOp::Ge: { return vm.makeBool(!lt); } break;
            default: { } break;
        }
    }
    return Object::binary(vm, op, self, rhs);
}

inline Handle ListVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    if (name == "append")
        return makeMethod(vm, "append", {"item"},
            [self](KiritoVM& vm, std::span<const Handle> args) -> Handle {
                if (args.size() != 1) throw KiritoError("append expected 1 argument");
                static_cast<ListVal&>(vm.arena().deref(self)).append(vm.arena(), args[0]);
                return vm.none();
            },
            std::vector<Handle>{self});
    if (name == "pop")
        return makeMethod(vm, "pop", {"index"},
            [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                auto& list = static_cast<ListVal&>(vm.arena().deref(self));
                if (list.elems.empty()) throw KiritoError("pop from empty List");
                // pop() removes the last element; pop(i) removes (and returns) index i (negative
                // counts from the end).
                int64_t idx = static_cast<int64_t>(list.elems.size()) - 1;
                if (!a.empty()) {
                    const Object& o = vm.arena().deref(a[0]);
                    if (o.kind() != ValueKind::Integer) throw KiritoError("pop index must be an Integer");
                    idx = static_cast<const IntVal&>(o).value();
                    if (idx < 0) idx += static_cast<int64_t>(list.elems.size());
                    if (idx < 0 || idx >= static_cast<int64_t>(list.elems.size()))
                        throw KiritoError("pop index out of range");
                }
                Handle v = list.elems[static_cast<std::size_t>(idx)];
                list.elems.erase(list.elems.begin() + idx);
                return v;
            },
            std::vector<Handle>{self});
    auto self_list = [](KiritoVM& vm, Handle self) -> ListVal& {
        return static_cast<ListVal&>(vm.arena().deref(self));
    };
    // apply(fn) — a new List with `fn` applied to each element (like tensor.apply: same type out).
    if (name == "apply")
        return makeMethod(vm, "apply", {"fn"}, [self, self_list](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) throw KiritoError("apply expects a function");
            Handle fn = a[0];
            std::vector<Handle> src = self_list(vm, self).elems;   // snapshot: fn must not see the result
            RootScope rs(vm);
            rs.addAll(src);   // fn may clear THIS list + allocate; keep the snapshot elements alive
            auto out = std::make_unique<ListVal>();
            out->elems.reserve(src.size());
            for (Handle h : src) {
                std::array<Handle, 1> args{h};
                out->elems.push_back(rs.add(vm.arena().deref(fn).call(vm, args)));
            }
            return vm.alloc(std::move(out));
        }, std::vector<Handle>{self});
    if (name == "reverse")
        return makeMethod(vm,
            "reverse", {}, [self, self_list](KiritoVM& vm, std::span<const Handle>) -> Handle {
                auto& e = self_list(vm, self).elems;
                std::reverse(e.begin(), e.end());
                return vm.none();
            }, std::vector<Handle>{self});
    if (name == "sort")
        return makeMethod(vm,
            "sort", {"key", "reverse"}, [self, self_list](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                // sort([key][, reverse]) — STABLE in-place sort. `key` is an optional callable
                // mapping each element to a sort key; `reverse` (a truthy 2nd arg) descends.
                Handle keyFn{};
                bool hasKey = false, reverse = false;
                if (!a.empty() && vm.arena().deref(a[0]).kind() != ValueKind::None) {
                    keyFn = a[0];
                    hasKey = true;
                }
                if (a.size() > 1) reverse = vm.arena().deref(a[1]).truthy();
                // Snapshot the elements first: the `key` fn and the `_lt_` comparator run arbitrary
                // Kirito code that may mutate THIS list (append/clear), which would realloc `elems`
                // and dangle a cached reference/iterator (a UAF). We sort the snapshot and only
                // write the result back once all user code has run — mirroring `apply`.
                std::vector<Handle> src = self_list(vm, self).elems;
                // Precompute keys once per element (Schwartzian transform): avoids re-invoking the
                // key function O(n log n) times and keeps the comparator allocation-free. The keys
                // are GC-rooted for the duration of the sort.
                RootScope rs(vm);
                rs.addAll(src);   // key/_lt_ may clear THIS list + allocate; keep the snapshot alive
                std::vector<std::pair<Handle, Handle>> tagged;  // (key, element)
                tagged.reserve(src.size());
                for (Handle el : src) {
                    Handle k = el;
                    if (hasKey) {
                        std::array<Handle, 1> args{el};
                        k = rs.add(vm.arena().deref(keyFn).call(vm, args));
                    }
                    tagged.emplace_back(k, el);
                }
                std::stable_sort(tagged.begin(), tagged.end(),
                                 [&](const std::pair<Handle, Handle>& x, const std::pair<Handle, Handle>& y) {
                                     return reverse ? kiLessThan(vm, y.first, x.first)
                                                    : kiLessThan(vm, x.first, y.first);
                                 });
                // Re-fetch the list (the handle is rooted, so the ListVal itself is stable) and
                // overwrite its contents with the sorted order.
                auto& e = self_list(vm, self).elems;
                e.clear();
                e.reserve(tagged.size());
                for (const auto& p : tagged) e.push_back(p.second);
                return vm.none();
            }, std::vector<Handle>{self});
    if (name == "insert")
        // minArgs=2: a keyword call skipping a required slot (insert(item=99)) throws
        // "missing required argument 'index'" rather than None-filling it into a misleading
        // "insert expects an Integer" downstream (A05-2).
        return makeMethod(vm,
            "insert", {"index", "item"}, [self, self_list](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                if (a.size() < 2) throw KiritoError("insert expects (index, item)");
                auto& list = self_list(vm, self);
                int64_t i = argInt(vm, a[0], "insert");  // checked: a non-Integer index is a clean error, not a bad downcast
                if (i < 0) i += static_cast<int64_t>(list.elems.size());
                if (i < 0) i = 0;
                if (i > static_cast<int64_t>(list.elems.size())) i = static_cast<int64_t>(list.elems.size());
                list.insertElem(vm.arena(), static_cast<std::size_t>(i), a[1]);
                return vm.none();
            }, std::vector<Handle>{self}, 2);
    if (name == "remove")
        return makeMethod(vm,
            "remove", {"value"}, [self, self_list](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                if (a.empty()) throw KiritoError("remove expects a value");
                // Find in a GC-rooted snapshot: kiEquals may run a user _eq_ that reallocs/mutates this
                // List, dangling a live `elems` reference (A09-1). Erase from the LIVE list, bounds-checked.
                RootScope rs(vm);
                std::vector<Handle> snap = self_list(vm, self).elems;
                for (Handle h : snap) rs.add(h);
                for (std::size_t i = 0; i < snap.size(); ++i)
                    if (kiEquals(vm, snap[i], a[0])) {
                        auto& e = self_list(vm, self).elems;
                        if (i < e.size()) e.erase(e.begin() + i);
                        return vm.none();
                    }
                throw KiritoError("remove: value not in List");
            }, std::vector<Handle>{self});
    if (name == "index")
        return makeMethod(vm,
            "index", {"value", "start", "end"}, [self, self_list](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                if (a.empty()) throw KiritoError("index expects a value");
                RootScope rs(vm);                                    // search a rooted snapshot (A09-1)
                std::vector<Handle> snap = self_list(vm, self).elems;
                for (Handle h : snap) rs.add(h);
                // Optional [start[, end]] search window (negatives count from the end).
                int64_t n = static_cast<int64_t>(snap.size()), start = 0, end = n;
                auto clampIdx = [&](Handle h, int64_t dflt) {
                    if (vm.arena().deref(h).kind() == ValueKind::None) return dflt;
                    int64_t k = argInt(vm, h, "index");
                    if (k < 0) k += n;
                    return k < 0 ? int64_t{0} : k > n ? n : k;
                };
                if (a.size() > 1) start = clampIdx(a[1], 0);
                if (a.size() > 2) end = clampIdx(a[2], n);
                for (int64_t i = start; i < end; ++i)
                    if (kiEquals(vm, snap[static_cast<std::size_t>(i)], a[0]))
                        return vm.makeInt(i);
                throw KiritoError("index: value not in List");
            }, std::vector<Handle>{self});
    if (name == "extend")
        return makeMethod(vm,
            "extend", {"iterable"}, [self, self_list](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                if (a.empty()) throw KiritoError("extend expects an iterable");
                auto other = vm.arena().deref(a[0]).iterate(vm);
                if (!other) throw KiritoError("extend expects an iterable");
                auto& list = self_list(vm, self);
                for (Handle h : other.value()) list.append(vm.arena(), h);
                return vm.none();
            }, std::vector<Handle>{self});
    if (name == "copy")
        return makeMethod(vm,
            "copy", {}, [self, self_list](KiritoVM& vm, std::span<const Handle>) -> Handle {
                // Shallow copy: a new List sharing the same element handles (aliasing preserved).
                auto c = std::make_unique<ListVal>();
                c->elems = self_list(vm, self).elems;
                return vm.alloc(std::move(c));
            }, std::vector<Handle>{self});
    if (name == "clear")
        return makeMethod(vm,
            "clear", {}, [self, self_list](KiritoVM& vm, std::span<const Handle>) -> Handle {
                self_list(vm, self).elems.clear();
                return vm.none();
            }, std::vector<Handle>{self});
    if (name == "count")
        return makeMethod(vm,
            "count", {"value"}, [self, self_list](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                if (a.size() != 1) throw KiritoError("count expected 1 argument");
                RootScope rs(vm);                                    // count over a rooted snapshot (A09-1)
                std::vector<Handle> snap = self_list(vm, self).elems;
                for (Handle h : snap) rs.add(h);
                int64_t n = 0;
                for (Handle h : snap)
                    if (kiEquals(vm, h, a[0])) ++n;
                return vm.makeInt(n);
            }, std::vector<Handle>{self});
    return Object::getAttr(vm, self, name);
}

inline Handle DictVal::getItem(KiritoVM& vm, std::span<const Handle> keys) {
    Handle key = singleKey(*this, keys);
    const Handle* v = find(vm.arena(), key);
    if (!v) throw KiritoError("key not found: " + vm.stringify(key));
    return *v;
}
inline void DictVal::setItem(KiritoVM& vm, std::span<const Handle> keys, Handle value) {
    Handle key = singleKey(*this, keys);
    set(vm.arena(), key, value);
}
inline Handle DictVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
        return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
    };
    // Like bind but with `req` leading REQUIRED params: a keyword call that skips one (e.g.
    // d.setdefault(default=7)) errors instead of silently passing None as the key (A05-2).
    auto bindReq = [&](const char* nm, std::size_t req, std::vector<std::string> params, NativeFn fn) {
        return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self}, req);
    };
    auto dict = [](KiritoVM& vm, Handle self) -> DictVal& {
        return static_cast<DictVal&>(vm.arena().deref(self));
    };
    // apply(fn) — a new Dict with the same keys and `fn` applied to each value (like tensor.apply).
    if (name == "apply")
        return bind("apply", {"fn"}, [self, dict](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) throw KiritoError("apply expects a function");
            Handle fn = a[0];
            auto pairs = dict(vm, self).pairs();                  // snapshot
            RootScope rs(vm);
            for (auto& [k, v] : pairs) { rs.add(k); rs.add(v); }  // fn may clear THIS dict + allocate
            auto out = std::make_unique<DictVal>();
            for (auto& [k, v] : pairs) {
                std::array<Handle, 1> args{v};
                Handle nv = rs.add(vm.arena().deref(fn).call(vm, args));
                out->set(vm.arena(), k, nv);
            }
            return vm.alloc(std::move(out));
        });
    if (name == "keys")
        return bind("keys", {}, [self, dict](KiritoVM& vm, std::span<const Handle>) -> Handle {
            RootScope rs(vm);
            auto list = std::make_unique<ListVal>();
            for (Handle k : dict(vm, self).keys()) list->elems.push_back(rs.add(k));
            return vm.alloc(std::move(list));
        });
    if (name == "values")
        return bind("values", {}, [self, dict](KiritoVM& vm, std::span<const Handle>) -> Handle {
            RootScope rs(vm);
            auto list = std::make_unique<ListVal>();
            for (const auto& [k, v] : dict(vm, self).pairs()) list->elems.push_back(rs.add(v));
            return vm.alloc(std::move(list));
        });
    if (name == "items")
        return bind("items", {}, [self, dict](KiritoVM& vm, std::span<const Handle>) -> Handle {
            RootScope rs(vm);
            auto list = std::make_unique<ListVal>();
            for (const auto& [k, v] : dict(vm, self).pairs()) {   // pairs(): one walk, no per-key re-probe
                auto pair = std::make_unique<ListVal>();
                pair->elems.push_back(k);
                pair->elems.push_back(v);
                list->elems.push_back(rs.add(vm.alloc(std::move(pair))));
            }
            return vm.alloc(std::move(list));
        });
    if (name == "get")
        return bindReq("get", 1, {"key", "default"}, [self, dict](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) throw KiritoError("get expects a key");
            const Handle* v = dict(vm, self).find(vm.arena(), a[0]);
            if (v) return *v;
            return a.size() > 1 ? a[1] : vm.none();
        });
    if (name == "pop")
        return bindReq("pop", 1, {"key", "default"}, [self, dict](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) throw KiritoError("pop expects a key");
            auto& d = dict(vm, self);
            const Handle* v = d.find(vm.arena(), a[0]);
            if (!v) {
                if (a.size() > 1) return a[1];
                throw KiritoError("pop: key not found");
            }
            Handle result = *v;
            d.remove(vm.arena(), a[0]);
            return result;
        });
    if (name == "remove")
        return bind("remove", {"key"}, [self, dict](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) throw KiritoError("remove expects a key");
            // Like pop, throw on a missing key (the doc says "throws if absent; like pop but
            // returns nothing"). DictVal::remove returns a bool telling whether anything was deleted.
            if (!dict(vm, self).remove(vm.arena(), a[0]))
                throw KiritoError("key not found: " + vm.stringify(a[0]));
            return vm.none();
        });
    if (name == "copy")
        return bind("copy", {}, [self, dict](KiritoVM& vm, std::span<const Handle>) -> Handle {
            auto c = std::make_unique<DictVal>();
            c->buckets = dict(vm, self).buckets;
            c->count = dict(vm, self).count;
            return vm.alloc(std::move(c));
        });
    if (name == "clear")
        return bind("clear", {}, [self, dict](KiritoVM& vm, std::span<const Handle>) -> Handle {
            dict(vm, self).clear();  // guarded: rejects a reentrant clear mid-probe (double-free)
            return vm.none();
        });
    if (name == "update")
        return bind("update", {"other"}, [self, dict](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            // update(other): merge another Dict, or an iterable of [key, value] pairs (entries override).
            if (a.size() != 1) throw KiritoError("update expected 1 argument");
            Object& o = vm.arena().deref(a[0]);
            if (o.kind() == ValueKind::Dict) {
                for (const auto& [k, v] : static_cast<const DictVal&>(o).pairs())
                    dict(vm, self).set(vm.arena(), k, v);
                return vm.none();
            }
            // Root every level: a user _iter_ may hand back a freshly built container that nothing
            // else roots, and DictVal::set runs a user _hash_/_eq_ that allocates — so an unrooted
            // pair (or its key/value) is swept mid-loop (dangling handle). The set-algebra family
            // roots the same way; rootedIterate is the single source of that idiom.
            RootScope rs(vm);
            auto pairs = rootedIterate(vm, a[0], rs, "update expects a Dict or an iterable of [key, value] pairs");
            for (Handle pairH : pairs) {
                auto pit = rootedIterate(vm, pairH, rs, "update: each pair must have exactly 2 elements (key, value)");
                if (pit.size() != 2)
                    throw KiritoError("update: each pair must have exactly 2 elements (key, value)");
                dict(vm, self).set(vm.arena(), pit[0], pit[1]);
            }
            return vm.none();
        });
    if (name == "setdefault")
        return bindReq("setdefault", 1, {"key", "default"}, [self, dict](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            // setdefault(key[, default]): return existing value, else insert default (or None).
            if (a.empty()) throw KiritoError("setdefault expected at least 1 argument");
            auto& d = dict(vm, self);
            const Handle* v = d.find(vm.arena(), a[0]);
            if (v) return *v;
            Handle dflt = a.size() > 1 ? a[1] : vm.none();
            d.set(vm.arena(), a[0], dflt);
            return dflt;
        });
    if (name == "popitem")
        return bind("popitem", {}, [self, dict](KiritoVM& vm, std::span<const Handle>) -> Handle {
            auto& d = dict(vm, self);
            RootScope rs(vm);
            auto kv = d.popArbitrary();   // takes the pair from its bucket; see the comment there
            Handle k = rs.add(kv.first);  // (a NaN key can't be looked back up, and this drains it)
            Handle v = rs.add(kv.second);
            auto pair = std::make_unique<ListVal>();
            pair->elems.push_back(k);
            pair->elems.push_back(v);
            return vm.alloc(std::move(pair));
        });
    return Object::getAttr(vm, self, name);
}

inline Handle SetVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    // apply(fn) — a new Set with `fn` applied to each element (like tensor.apply: same type out;
    // transformed elements that collide collapse, as in any Set).
    if (name == "apply")
        return makeMethod(vm, "apply", {"fn"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) throw KiritoError("apply expects a function");
            Handle fn = a[0];
            std::vector<Handle> src = static_cast<SetVal&>(vm.arena().deref(self)).items();  // snapshot
            RootScope rs(vm);
            rs.addAll(src);   // fn may clear THIS set + allocate; keep the snapshot elements alive
            auto out = std::make_unique<SetVal>();
            for (Handle h : src) {
                std::array<Handle, 1> args{h};
                out->add(vm.arena(), rs.add(vm.arena().deref(fn).call(vm, args)));
            }
            return vm.alloc(std::move(out));
        }, std::vector<Handle>{self});
    if (name == "add")
        return makeMethod(vm, "add", {"value"},
            [self](KiritoVM& vm, std::span<const Handle> args) -> Handle {
                if (args.size() != 1) throw KiritoError("add expected 1 argument");
                static_cast<SetVal&>(vm.arena().deref(self)).add(vm.arena(), args[0]);
                return vm.none();
            },
            std::vector<Handle>{self});
    if (name == "contains")
        return makeMethod(vm, "contains", {"value"},
            [self](KiritoVM& vm, std::span<const Handle> args) -> Handle {
                if (args.size() != 1) throw KiritoError("contains expected 1 argument");
                return vm.makeBool(static_cast<SetVal&>(vm.arena().deref(self)).contains(vm.arena(), args[0]));
            },
            std::vector<Handle>{self});
    auto set_of = [](KiritoVM& vm, Handle h) -> SetVal& { return static_cast<SetVal&>(vm.arena().deref(h)); };
    auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
        return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
    };
    if (name == "remove")
        return bind("remove", {"value"}, [self, set_of](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) throw KiritoError("remove expects a value");
            auto& s = set_of(vm, self);
            const Object& v = vm.arena().deref(a[0]);
            if (!v.hashable()) throw KiritoError("unhashable type '" + v.typeName() + "'");  // match the
                // canonical message used by Set.add / Dict / hash() — Set.remove had drifted to a bare
                // "unhashable type" with no type name.
            std::size_t h = v.hash();
            if (s.probing_) throw KiritoError("Set changed size during a value comparison");
            ProbeScope guard(s.probing_);  // reentrant _eq_ must not realloc the bucket we hold
            auto it = s.buckets.find(h);
            if (it != s.buckets.end()) {
                auto i = probeBucket(vm.arena(), it->second, v, setKeyOf);
                if (i >= 0) {
                    it->second.erase(it->second.begin() + i); --s.count;
                    if (it->second.empty()) s.buckets.erase(it);   // reclaim the emptied bucket (A08-4)
                    return vm.none();
                }
            }
            throw KiritoError("remove: value not in Set");
        });
    if (name == "copy")
        return bind("copy", {}, [self, set_of](KiritoVM& vm, std::span<const Handle>) -> Handle {
            auto c = std::make_unique<SetVal>();
            c->buckets = set_of(vm, self).buckets;
            c->count = set_of(vm, self).count;
            return vm.alloc(std::move(c));
        });
    if (name == "discard")  // remove if present, no error otherwise (cf. remove)
        return bind("discard", {"value"}, [self, set_of](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) throw KiritoError("discard expects a value");
            auto& s = set_of(vm, self);
            const Object& v = vm.arena().deref(a[0]);
            if (!v.hashable()) return vm.none();
            std::size_t h = v.hash();
            if (s.probing_) throw KiritoError("Set changed size during a value comparison");
            ProbeScope guard(s.probing_);  // reentrant _eq_ must not realloc the bucket we hold
            auto it = s.buckets.find(h);
            if (it != s.buckets.end()) {
                auto i = probeBucket(vm.arena(), it->second, v, setKeyOf);
                if (i >= 0) {
                    it->second.erase(it->second.begin() + i); --s.count;
                    if (it->second.empty()) s.buckets.erase(it);   // reclaim the emptied bucket (A08-4)
                }
            }
            return vm.none();
        });
    if (name == "clear")
        return bind("clear", {}, [self, set_of](KiritoVM& vm, std::span<const Handle>) -> Handle {
            set_of(vm, self).clear();  // guarded: rejects a reentrant clear mid-probe (double-free)
            return vm.none();
        });
    if (name == "pop")  // remove and return an arbitrary element
        return bind("pop", {}, [self, set_of](KiritoVM& vm, std::span<const Handle>) -> Handle {
            return set_of(vm, self).popArbitrary();  // guarded like clear()
        });
    if (name == "union" || name == "intersection" || name == "difference" ||
        name == "symmetricdifference") {
        std::string op(name);
        return bind(op.c_str(), {"other"}, [self, set_of, op](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) throw KiritoError(op + " expects an iterable");
            RootScope rs(vm);
            auto result = std::make_unique<SetVal>();
            auto& s = set_of(vm, self);
            auto other = vm.arena().deref(a[0]).iterate(vm);
            if (!other) throw KiritoError(op + " expects an iterable");
            // GC-root the iterated elements: a String/Bytes/large-range `other` yields FRESHLY
            // allocated handles that are otherwise reachable from no root, so the trailing vm.alloc
            // (which may collect) could sweep them out of the not-yet-arena'd `result` (a UAF).
            for (Handle e : other.value()) rs.add(e);
            if (op == "union") {
                for (Handle e : s.items()) result->add(vm.arena(), e);
                for (Handle e : other.value()) result->add(vm.arena(), e);
            } else if (op == "intersection") {
                SetVal otherSet;
                for (Handle e : other.value()) otherSet.add(vm.arena(), e);
                for (Handle e : s.items()) if (otherSet.contains(vm.arena(), e)) result->add(vm.arena(), e);
            } else if (op == "difference") {
                SetVal otherSet;
                for (Handle e : other.value()) otherSet.add(vm.arena(), e);
                for (Handle e : s.items()) if (!otherSet.contains(vm.arena(), e)) result->add(vm.arena(), e);
            } else {  // symmetricdifference: in one but not both
                SetVal otherSet;
                for (Handle e : other.value()) otherSet.add(vm.arena(), e);
                for (Handle e : s.items()) if (!otherSet.contains(vm.arena(), e)) result->add(vm.arena(), e);
                for (Handle e : otherSet.items()) if (!s.contains(vm.arena(), e)) result->add(vm.arena(), e);
            }
            return vm.alloc(std::move(result));
        });
    }
    if (name == "issubset" || name == "issuperset" || name == "isdisjoint") {
        std::string op(name);
        return bind(op.c_str(), {"other"}, [self, set_of, op](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) throw KiritoError(op + " expects an iterable");
            auto& s = set_of(vm, self);
            auto other = vm.arena().deref(a[0]).iterate(vm);
            if (!other) throw KiritoError(op + " expects an iterable");
            // Root `other`'s elements: iterating a String/Bytes/range yields FRESHLY allocated handles
            // reachable from no GC root, and `otherSet.add` can collect (a user _hash_/_eq_, or the
            // pool) between adds — the same rooting the union family does (A08-5). `otherSet` is a
            // stack local invisible to the GC, so the elements live in `rs` for the branch's duration.
            RootScope rs(vm);
            for (Handle e : other.value()) rs.add(e);
            SetVal otherSet;
            for (Handle e : other.value()) otherSet.add(vm.arena(), e);
            if (op == "issubset") {
                for (Handle e : s.items()) if (!otherSet.contains(vm.arena(), e)) return vm.makeBool(false);
                return vm.makeBool(true);
            }
            if (op == "issuperset") {
                for (Handle e : otherSet.items()) if (!s.contains(vm.arena(), e)) return vm.makeBool(false);
                return vm.makeBool(true);
            }
            for (Handle e : s.items()) if (otherSet.contains(vm.arena(), e)) return vm.makeBool(false);  // isdisjoint
            return vm.makeBool(true);
        });
    }
    return Object::getAttr(vm, self, name);
}

// Set algebra via operators. Kirito has no |/&/^ tokens, so the natural operators are `-` (difference)
// and the ordering comparisons as (proper) subset/superset. All require a Set on
// the right; anything else defers to the base (a clear "unsupported operator" error). union/
// intersection/symmetricdifference stay as methods; ==/!= go through equals().
inline Handle SetVal::binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) {
    const Object& ro = vm.arena().deref(rhs);
    if (ro.kind() != ValueKind::Set) return Object::binary(vm, op, self, rhs);
    const SetVal& a = static_cast<const SetVal&>(vm.arena().deref(self));
    const SetVal& b = static_cast<const SetVal&>(ro);
    auto subset = [&](const SetVal& x, const SetVal& y) {        // x is a subset of y
        for (Handle e : x.items()) if (!y.contains(vm.arena(), e)) return false;
        return true;
    };
    switch (op) {
        case BinOp::Sub: {                                      // difference: in a, not in b
            RootScope rs(vm);
            auto r = std::make_unique<SetVal>();
            for (Handle e : a.items()) if (!b.contains(vm.arena(), e)) r->add(vm.arena(), e);
            return vm.alloc(std::move(r));
        } break;
        case BinOp::Le: { return vm.makeBool(subset(a, b)); } break;                          // a <= b
        case BinOp::Ge: { return vm.makeBool(subset(b, a)); } break;                          // a >= b
        case BinOp::Lt: { return vm.makeBool(a.count < b.count && subset(a, b)); } break;     // a < b (proper)
        case BinOp::Gt: { return vm.makeBool(b.count < a.count && subset(b, a)); } break;     // a > b (proper)
        default: { return Object::binary(vm, op, self, rhs); } break;
    }
}

// --- slicing helper --------------------------------------------------------------------------

// --- String indexing / slicing / iteration (UTF-8 aware) -------------------------------------

inline Handle StrVal::getItem(KiritoVM& vm, std::span<const Handle> keys) {
    if (isAscii()) {  // O(1): code-point index == byte index
        std::size_t i = sequenceIndex(vm, value_.size(), singleKey(*this, keys));
        return vm.makeString(value_.substr(i, 1));
    }
    const auto& starts = codePointStarts();  // cached
    std::size_t i = sequenceIndex(vm, starts.size(), singleKey(*this, keys));
    std::size_t b = starts[i];
    std::size_t e = (i + 1 < starts.size()) ? starts[i + 1] : value_.size();
    return vm.makeString(value_.substr(b, e - b));
}

inline Handle StrVal::slice(KiritoVM& vm, Handle s, Handle e, Handle st) {
    if (isAscii()) {  // byte slice == code-point slice
        int64_t len = static_cast<int64_t>(value_.size());
        std::string out;
        for (int64_t i : sliceIndices(vm, len, s, e, st)) out += value_[static_cast<std::size_t>(i)];
        return vm.makeString(std::move(out));
    }
    const auto& starts = codePointStarts();
    int64_t len = static_cast<int64_t>(starts.size());
    std::string out;
    for (int64_t i : sliceIndices(vm, len, s, e, st)) {
        std::size_t b = starts[i];
        std::size_t en = (i + 1 < len) ? starts[i + 1] : value_.size();
        out.append(value_, b, en - b);
    }
    return vm.makeString(std::move(out));
}

inline std::optional<std::vector<Handle>> StrVal::iterate(KiritoVM& vm) {
    RootScope rs(vm);  // each character is a fresh String; protect them while building
    std::vector<Handle> out;
    if (isAscii()) {
        out.reserve(value_.size());
        for (char c : value_) out.push_back(rs.add(vm.makeString(std::string(1, c))));
        return out;
    }
    const auto& starts = codePointStarts();
    out.reserve(starts.size());
    for (std::size_t i = 0; i < starts.size(); ++i) {
        std::size_t b = starts[i];
        std::size_t e = (i + 1 < starts.size()) ? starts[i + 1] : value_.size();
        out.push_back(rs.add(vm.makeString(value_.substr(b, e - b))));
    }
    return out;
}

inline bool StrVal::contains(KiritoVM& vm, Handle value) {
    const Object& o = vm.arena().deref(value);
    if (o.kind() != ValueKind::String)
        throw KiritoError("'in <String>' requires a String, not '" + o.typeName() + "'");
    return value_.find(static_cast<const StrVal&>(o).value()) != std::string::npos;
}

// Read a String argument or throw.
inline const std::string& asStr(KiritoVM& vm, Handle h, const char* what) {
    const Object& o = vm.arena().deref(h);
    if (o.kind() != ValueKind::String) throw KiritoError(std::string(what) + " requires a String");
    return static_cast<const StrVal&>(o).value();
}

// Map a code-point index (negative counts from the end, out-of-range clamps) to a byte
// offset into a UTF-8 string — for the optional start/end of the search methods.
inline std::size_t cpIndexToByte(const std::string& s, int64_t cp) {
    auto starts = utf8Starts(s);
    int64_t n = static_cast<int64_t>(starts.size());
    if (cp < 0) cp += n;
    if (cp < 0) cp = 0;
    if (cp >= n) return s.size();
    return starts[static_cast<std::size_t>(cp)];
}

// Decode a UTF-8 string to its code points (so edit distance is by character, not byte).
inline std::vector<uint32_t> strCodepoints(const std::string& s) {
    std::vector<uint32_t> cps;
    for (std::size_t st : utf8Starts(s)) cps.push_back(utf8DecodeAt(s, st));
    return cps;
}

// Levenshtein (edit) distance between two code-point sequences — the classic O(m·n) dynamic
// program with two rolling rows (O(min(m,n)) memory). Insert/delete/substitute each cost 1.
inline int64_t levenshteinDistance(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    if (a.empty()) return static_cast<int64_t>(b.size());
    if (b.empty()) return static_cast<int64_t>(a.size());
    // Iterate over the longer string's columns with the shorter along the rows to bound memory.
    const std::vector<uint32_t>& s = a.size() <= b.size() ? a : b;
    const std::vector<uint32_t>& t = a.size() <= b.size() ? b : a;
    std::size_t m = s.size(), n = t.size();
    std::vector<int64_t> prev(m + 1), cur(m + 1);
    for (std::size_t i = 0; i <= m; ++i) prev[i] = static_cast<int64_t>(i);
    for (std::size_t j = 1; j <= n; ++j) {
        cur[0] = static_cast<int64_t>(j);
        for (std::size_t i = 1; i <= m; ++i) {
            int64_t cost = (s[i - 1] == t[j - 1]) ? 0 : 1;
            cur[i] = std::min({prev[i] + 1, cur[i - 1] + 1, prev[i - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

inline Handle StrVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
        return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
    };
    // Like `bind`, but enforces a minimum positional arity before the impl runs. makeMethod's
    // positional fast path forwards the call args verbatim, so a method that dereferences a[0]/a[1]
    // would read past an empty span on an under-arity call (UB / nondeterministic). Methods with a
    // required leading argument use this so a missing arg is a clean, deterministic error.
    auto bindReq = [&](std::string nm, std::size_t minArgs, std::vector<std::string> params, NativeFn fn) {
        NativeFn guarded = [nm, minArgs, fn](KiritoVM& v, std::span<const Handle> a) -> Handle {
            if (a.size() < minArgs)
                throw KiritoError(nm + "() expected at least " + std::to_string(minArgs) + " argument(s)");
            return fn(v, a);
        };
        // Pass minArgs to makeMethod too: the positional guard above catches an under-arity POSITIONAL
        // call, but a keyword call that skips a required leading arg (e.g. `m.replace(new="x")`) is
        // caught inside makeMethod before it None-fills the hole. (A05-2.)
        return makeMethod(vm, nm, std::move(params), std::move(guarded), std::vector<Handle>{self}, minArgs);
    };
    auto recv = [](KiritoVM& vm, Handle self) -> const std::string& {
        return static_cast<StrVal&>(vm.arena().deref(self)).value();
    };
    // Resolve the optional [start[, end]] arguments (args[from] onward) to a byte [lo, hi) window.
    auto window = [&](KiritoVM& vm, std::span<const Handle> a, std::size_t from, const std::string& s)
        -> std::pair<std::size_t, std::size_t> {
        std::size_t lo = 0, hi = s.size();
        if (a.size() > from && vm.arena().deref(a[from]).kind() != ValueKind::None)
            lo = cpIndexToByte(s, argInt(vm, a[from], "start"));
        if (a.size() > from + 1 && vm.arena().deref(a[from + 1]).kind() != ValueKind::None)
            hi = cpIndexToByte(s, argInt(vm, a[from + 1], "end"));
        if (hi < lo) hi = lo;
        return {lo, hi};
    };
    // Byte offset -> code-point index (for returning a position).
    auto byteToCp = [](const std::string& s, std::size_t bp) -> int64_t {
        int64_t cp = 0;
        for (std::size_t st : utf8Starts(s)) { if (st >= bp) break; ++cp; }
        return cp;
    };

    // Code-point-aware case conversion (handles ASCII + Latin-1 + Latin Extended-A, so Polish and
    // most European text map correctly, not just ASCII).
    auto mapCase = [](const std::string& s, unsigned (*fn)(unsigned)) {
        std::string out;
        out.reserve(s.size());
        for (std::size_t st : utf8Starts(s)) utf8Encode(fn(utf8DecodeAt(s, st)), out);
        return out;
    };
    if (name == "encode")
        return bind("encode", {"encoding"}, [self, recv](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string enc = "utf-8";
            if (!a.empty() && vm.arena().deref(a[0]).kind() != ValueKind::None) enc = asStr(vm, a[0], "encode");
            return vm.alloc(std::make_unique<BytesVal>(bytesutil::encode(recv(vm, self), enc)));
        });
    // apply(fn) — a new String with `fn` applied to each character (fn takes/returns a String).
    if (name == "apply")
        return bindReq("apply", 1, {"fn"}, [self, recv](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Handle fn = a[0];
            std::string s = recv(vm, self);                  // copy: immutable, GC-safe across calls
            std::vector<std::size_t> starts = utf8Starts(s);
            std::string out;
            for (std::size_t i = 0; i < starts.size(); ++i) {
                std::size_t b = starts[i], e = (i + 1 < starts.size()) ? starts[i + 1] : s.size();
                RootScope rs(vm);                            // root the per-char arg + result across the call
                std::array<Handle, 1> args{rs.add(vm.makeString(s.substr(b, e - b)))};
                const Object& r = vm.arena().deref(rs.add(vm.arena().deref(fn).call(vm, args)));
                if (r.kind() != ValueKind::String) throw KiritoError("String apply: result must be a String");
                out += static_cast<const StrVal&>(r).value();
            }
            return vm.makeString(std::move(out));
        });
    if (name == "upper")
        return bind("upper", {}, [self, recv, mapCase](KiritoVM& vm, std::span<const Handle>) {
            return vm.makeString(mapCase(recv(vm, self), utf8ToUpperCp));
        });
    if (name == "lower")
        return bind("lower", {}, [self, recv, mapCase](KiritoVM& vm, std::span<const Handle>) {
            return vm.makeString(mapCase(recv(vm, self), utf8ToLowerCp));
        });
    if (name == "strip" || name == "lstrip" || name == "rstrip") {
        bool left = name != "rstrip", right = name != "lstrip";
        return bind(std::string(name).c_str(), {"chars"}, [self, recv, left, right](KiritoVM& vm, std::span<const Handle> a) {
            const std::string& s = recv(vm, self);
            // With an optional `chars` argument, trim any of those characters; otherwise whitespace.
            // Match by CODE POINT (not byte) so a multibyte char in `chars` can't peel a shared
            // continuation byte off a multibyte char in `s` and corrupt the UTF-8.
            bool hasChars = !a.empty() && vm.arena().deref(a[0]).kind() != ValueKind::None;
            std::string chars = hasChars ? asStr(vm, a[0], "strip") : std::string();
            std::vector<unsigned> stripCps;
            if (hasChars) for (std::size_t st : utf8Starts(chars)) stripCps.push_back(utf8DecodeAt(chars, st));
            auto isTrim = [&](unsigned cp) {
                if (hasChars) return std::find(stripCps.begin(), stripCps.end(), cp) != stripCps.end();
                return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v';
            };
            auto starts = utf8Starts(s);
            std::size_t n = starts.size(), lo = 0, hi = n;
            if (left) while (lo < hi && isTrim(utf8DecodeAt(s, starts[lo]))) ++lo;
            if (right) while (hi > lo && isTrim(utf8DecodeAt(s, starts[hi - 1]))) --hi;
            std::size_t b = (lo < n) ? starts[lo] : s.size();
            std::size_t e = (hi < n) ? starts[hi] : s.size();
            return vm.makeString(s.substr(b, e - b));
        });
    }
    if (name == "startswith")
        return bindReq("startswith", 1, {"prefix", "start", "end"}, [self, recv, window](KiritoVM& vm, std::span<const Handle> a) {
            const std::string& s = recv(vm, self);
            const std::string& p = asStr(vm, a[0], "startswith");
            auto [lo, hi] = window(vm, a, 1, s);
            return vm.makeBool(lo + p.size() <= hi && s.compare(lo, p.size(), p) == 0);
        });
    if (name == "endswith")
        return bindReq("endswith", 1, {"suffix", "start", "end"}, [self, recv, window](KiritoVM& vm, std::span<const Handle> a) {
            const std::string& s = recv(vm, self);
            const std::string& p = asStr(vm, a[0], "endswith");
            auto [lo, hi] = window(vm, a, 1, s);
            return vm.makeBool(hi >= p.size() && hi - p.size() >= lo && s.compare(hi - p.size(), p.size(), p) == 0);
        });
    if (name == "replace")
        return bindReq("replace", 2, {"old", "new", "count"}, [self, recv](KiritoVM& vm, std::span<const Handle> a) {
            std::string s = recv(vm, self);
            const std::string& from = asStr(vm, a[0], "replace");
            const std::string& to = asStr(vm, a[1], "replace");
            // Optional count: replace at most that many occurrences (a negative or None means all).
            int64_t count = -1;
            if (a.size() > 2 && vm.arena().deref(a[2]).kind() != ValueKind::None)
                count = argInt(vm, a[2], "replace");
            if (from.empty() && count != 0) {
                // An empty pattern matches at every code-point boundary, so the
                // replacement is interleaved between characters (and at both ends).
                std::string out;
                int64_t done = 0;
                auto starts = utf8Starts(s);
                starts.push_back(s.size());  // boundary after the last character
                std::size_t prev = 0;
                for (std::size_t b : starts) {
                    out.append(s, prev, b - prev);
                    prev = b;
                    if (count >= 0 && done >= count) { break; }
                    out += to;
                    ++done;
                    if (out.size() > kMaxRepeat) throw KiritoError("replace result too large");
                }
                out.append(s, prev, std::string::npos);
                s = std::move(out);
            } else if (!from.empty() && count != 0) {
                std::string out;
                std::size_t pos = 0, prev = 0;
                int64_t done = 0;
                while ((pos = s.find(from, prev)) != std::string::npos) {
                    out.append(s, prev, pos - prev);
                    out += to;
                    if (out.size() > kMaxRepeat) throw KiritoError("replace result too large");
                    prev = pos + from.size();
                    if (count >= 0 && ++done >= count) break;
                }
                out.append(s, prev, std::string::npos);
                s = std::move(out);
            }
            return vm.makeString(std::move(s));
        });
    if (name == "count")
        return bindReq("count", 1, {"sub", "start", "end"}, [self, recv, window](KiritoVM& vm, std::span<const Handle> a) {
            const std::string& s = recv(vm, self);
            const std::string& sub = asStr(vm, a[0], "count");
            auto [lo, hi] = window(vm, a, 1, s);
            int64_t n = 0;
            if (sub.empty()) {
                // The empty string occurs at every code-point boundary in the window
                // (so "abc".count("") == 4 — before each character and after the last).
                auto starts = utf8Starts(s);
                starts.push_back(s.size());
                for (std::size_t b : starts)
                    if (b >= lo && b <= hi) ++n;
                return vm.makeInt(n);
            }
            for (std::size_t pos = s.find(sub, lo); pos != std::string::npos && pos + sub.size() <= hi;
                 pos = s.find(sub, pos + sub.size()))
                ++n;
            return vm.makeInt(n);
        });
    if (name == "find")
        return bindReq("find", 1, {"sub", "start", "end"}, [self, recv, window, byteToCp](KiritoVM& vm, std::span<const Handle> a) {
            const std::string& s = recv(vm, self);
            const std::string& sub = asStr(vm, a[0], "find");
            auto [lo, hi] = window(vm, a, 1, s);
            std::size_t bp = s.find(sub, lo);
            if (bp == std::string::npos || bp + sub.size() > hi) return vm.makeInt(-1);
            return vm.makeInt(byteToCp(s, bp));
        });
    if (name == "split")
        return bind("split", {"sep", "maxsplit"}, [self, recv](KiritoVM& vm, std::span<const Handle> a) {
            const std::string& s = recv(vm, self);
            // Optional maxsplit: at most that many splits (so at most maxsplit+1 fields); a None or
            // negative value means unlimited. The separator may be the first or, with no separator,
            // the second positional argument is the maxsplit.
            bool noSep = a.empty() || vm.arena().deref(a[0]).kind() == ValueKind::None;
            int64_t maxsplit = -1;
            std::size_t maxIdx = noSep ? (a.empty() ? 0 : 1) : 1;
            if (a.size() > maxIdx && vm.arena().deref(a[maxIdx]).kind() != ValueKind::None)
                maxsplit = argInt(vm, a[maxIdx], "split");
            RootScope rs(vm);
            auto list = std::make_unique<ListVal>();
            int64_t splits = 0;
            auto reached = [&] { return maxsplit >= 0 && splits >= maxsplit; };
            if (noSep) {
                std::size_t i = 0, n = s.size();
                while (i < n) {
                    while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
                    if (i >= n) break;
                    if (reached()) {  // remaining text is the final field — keep its trailing whitespace
                        list->elems.push_back(rs.add(vm.makeString(s.substr(i, n - i))));
                        break;
                    }
                    std::size_t start = i;
                    while (i < n && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
                    list->elems.push_back(rs.add(vm.makeString(s.substr(start, i - start))));
                    ++splits;
                }
            } else {
                const std::string& sep = asStr(vm, a[0], "split");
                if (sep.empty()) throw KiritoError("empty separator");
                std::size_t pos, prev = 0;
                while (!reached() && (pos = s.find(sep, prev)) != std::string::npos) {
                    list->elems.push_back(rs.add(vm.makeString(s.substr(prev, pos - prev))));
                    prev = pos + sep.size();
                    ++splits;
                }
                list->elems.push_back(rs.add(vm.makeString(s.substr(prev))));
            }
            return vm.alloc(std::move(list));
        });
    if (name == "join")
        return bindReq("join", 1, {"iterable"}, [self, recv](KiritoVM& vm, std::span<const Handle> a) {
            const std::string& sep = recv(vm, self);
            RootScope rs(vm);
            auto items = rootedIterate(vm, a[0], rs, "join expects an iterable");  // root: a user _str_ may allocate
            std::string out;
            bool first = true;
            for (Handle h : items) {
                if (!first) out += sep;
                first = false;
                out += asStr(vm, h, "join");
                if (out.size() > kMaxRepeat) throw KiritoError("join result too large");
            }
            return vm.makeString(std::move(out));
        });
    if (name == "format")
        return bind("format", {}, [self, recv](KiritoVM& vm, std::span<const Handle> a) {
            const std::string& tmpl = recv(vm, self);
            std::string out;
            std::size_t auto_i = 0;
            for (std::size_t i = 0; i < tmpl.size(); ++i) {
                if (tmpl[i] == '{') {
                    if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') { out += '{'; ++i; continue; }
                    std::size_t close = tmpl.find('}', i);
                    if (close == std::string::npos) throw KiritoError("unmatched '{' in format string");
                    std::string spec = tmpl.substr(i + 1, close - i - 1);
                    std::size_t idx;
                    if (spec.empty()) {
                        idx = auto_i++;
                    } else {
                        for (char ch : spec)
                            if (ch < '0' || ch > '9') throw KiritoError("format field must be an index");
                        try { idx = static_cast<std::size_t>(std::stoull(spec)); }
                        catch (const std::out_of_range&) { throw KiritoError("format index out of range"); }
                    }
                    if (idx >= a.size()) throw KiritoError("format index out of range");
                    out += vm.stringify(a[idx]);
                    i = close;
                } else if (tmpl[i] == '}' && i + 1 < tmpl.size() && tmpl[i + 1] == '}') {
                    out += '}';
                    ++i;
                } else {
                    out += tmpl[i];
                }
            }
            return vm.makeString(std::move(out));
        });
    // index(sub[, start[, end]]): like find but throws if not found.
    if (name == "index")
        return bindReq("index", 1, {"sub", "start", "end"}, [self, recv, window, byteToCp](KiritoVM& vm, std::span<const Handle> a) {
            const std::string& s = recv(vm, self);
            const std::string& sub = asStr(vm, a[0], "index");
            auto [lo, hi] = window(vm, a, 1, s);
            std::size_t bp = s.find(sub, lo);
            if (bp == std::string::npos || bp + sub.size() > hi) throw KiritoError("substring not found");
            return vm.makeInt(byteToCp(s, bp));
        });
    if (name == "rfind" || name == "rindex") {
        bool raise = name == "rindex";
        return bindReq(std::string(name), 1, {"sub", "start", "end"}, [self, recv, raise, window, byteToCp](KiritoVM& vm, std::span<const Handle> a) {
            const std::string& s = recv(vm, self);
            const std::string& sub = asStr(vm, a[0], "rfind");
            auto [lo, hi] = window(vm, a, 1, s);
            std::size_t bp = (hi >= sub.size()) ? s.rfind(sub, hi - sub.size()) : std::string::npos;
            if (bp == std::string::npos || bp < lo) {
                if (raise) throw KiritoError("substring not found");
                return vm.makeInt(-1);
            }
            return vm.makeInt(byteToCp(s, bp));
        });
    }
    // Character-class predicates (non-empty string, all code points satisfy the test).
    auto classify = [&](const char* nm, bool (*test)(unsigned)) {
        return bind(nm, {}, [self, recv, test](KiritoVM& vm, std::span<const Handle>) -> Handle {
            const std::string& s = recv(vm, self);
            if (s.empty()) return vm.makeBool(false);
            for (std::size_t st : utf8Starts(s)) if (!test(utf8DecodeAt(s, st))) return vm.makeBool(false);
            return vm.makeBool(true);
        });
    };
    if (name == "isdigit") return classify("isdigit", [](unsigned c) { return c >= '0' && c <= '9'; });
    if (name == "isalpha") return classify("isalpha", [](unsigned c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80; });  // non-ASCII letters count
    if (name == "isalnum") return classify("isalnum", [](unsigned c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80; });
    if (name == "isspace") return classify("isspace", [](unsigned c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; });
    // islower/isupper: ignore uncased chars, require >=1 cased char, and every
    // cased char must be of the requested case. (A char is "cased" if it has a different upper/lower form.)
    auto caseClassify = [&](const char* nm, bool wantLower) {
        return bind(nm, {}, [self, recv, wantLower](KiritoVM& vm, std::span<const Handle>) -> Handle {
            const std::string& s = recv(vm, self);
            bool hasCased = false;
            for (std::size_t st : utf8Starts(s)) {
                unsigned c = utf8DecodeAt(s, st);
                bool isLowerCased = utf8ToUpperCp(c) != c;   // has an uppercase form -> a lowercase letter
                bool isUpperCased = utf8ToLowerCp(c) != c;   // has a lowercase form -> an uppercase letter
                if (!isLowerCased && !isUpperCased) continue;  // uncased: ignore
                hasCased = true;
                if (wantLower ? !isLowerCased : !isUpperCased) return vm.makeBool(false);
            }
            return vm.makeBool(hasCased);
        });
    };
    if (name == "islower") return caseClassify("islower", true);
    if (name == "isupper") return caseClassify("isupper", false);
    // removeprefix / removesuffix.
    if (name == "removeprefix")
        return bindReq("removeprefix", 1, {"prefix"}, [self, recv](KiritoVM& vm, std::span<const Handle> a) {
            const std::string& s = recv(vm, self);
            const std::string& p = asStr(vm, a[0], "removeprefix");
            if (s.size() >= p.size() && s.compare(0, p.size(), p) == 0) return vm.makeString(s.substr(p.size()));
            return vm.makeString(s);
        });
    if (name == "removesuffix")
        return bindReq("removesuffix", 1, {"suffix"}, [self, recv](KiritoVM& vm, std::span<const Handle> a) {
            const std::string& s = recv(vm, self);
            const std::string& p = asStr(vm, a[0], "removesuffix");
            if (!p.empty() && s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0)
                return vm.makeString(s.substr(0, s.size() - p.size()));
            return vm.makeString(s);
        });
    // Padding/alignment by code-point width.
    if (name == "ljust" || name == "rjust" || name == "center") {
        std::string op(name);
        return bind(op.c_str(), {"width", "fillchar"}, [self, recv, op](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            const std::string& s = recv(vm, self);
            if (a.empty() || vm.arena().deref(a[0]).kind() != ValueKind::Integer)
                throw KiritoError(op + " expects an Integer width");
            int64_t width = static_cast<const IntVal&>(vm.arena().deref(a[0])).value();
            if (static_cast<uint64_t>(width < 0 ? 0 : width) > kMaxRepeat)
                throw KiritoError(op + " width too large");
            std::string fill = a.size() > 1 ? asStr(vm, a[1], op.c_str()) : " ";
            if (utf8Length(fill) != 1) throw KiritoError(op + " fill must be a single character");
            int64_t pad = width - static_cast<int64_t>(utf8Length(s));
            if (pad <= 0) return vm.makeString(s);
            // The width bound above is a CODE-POINT count, but a fill char is up to 4 UTF-8 bytes, so
            // bound the produced byte length too (else the buffer can reach ~4x the intended cap).
            if (static_cast<uint64_t>(pad) > kMaxRepeat / fill.size())
                throw KiritoError(op + " result too large");
            auto rep = [&](int64_t n) { std::string r; for (int64_t i = 0; i < n; ++i) r += fill; return r; };
            if (op == "ljust") return vm.makeString(s + rep(pad));
            if (op == "rjust") return vm.makeString(rep(pad) + s);
            return vm.makeString(rep(pad / 2) + s + rep(pad - pad / 2));  // center
        });
    }
    if (name == "zfill")
        return bind("zfill", {"width"}, [self, recv](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            const std::string& s = recv(vm, self);
            if (a.empty() || vm.arena().deref(a[0]).kind() != ValueKind::Integer)
                throw KiritoError("zfill expects an Integer width");
            int64_t width = static_cast<const IntVal&>(vm.arena().deref(a[0])).value();
            if (static_cast<uint64_t>(width < 0 ? 0 : width) > kMaxRepeat)
                throw KiritoError("zfill width too large");
            int64_t pad = width - static_cast<int64_t>(utf8Length(s));
            if (pad <= 0) return vm.makeString(s);
            // Keep a leading sign in front of the zero padding.
            std::string sign, body = s;
            if (!s.empty() && (s[0] == '+' || s[0] == '-')) { sign = s.substr(0, 1); body = s.substr(1); }
            return vm.makeString(sign + std::string(static_cast<std::size_t>(pad), '0') + body);
        });
    if (name == "partition" || name == "rpartition") {
        bool right = name == "rpartition";
        return bindReq(std::string(name), 1, {"sep"}, [self, recv, right](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            const std::string& s = recv(vm, self);
            const std::string& sep = asStr(vm, a[0], "partition");
            if (sep.empty()) throw KiritoError("empty separator");
            std::size_t pos = right ? s.rfind(sep) : s.find(sep);
            RootScope rs(vm);
            auto t = std::make_unique<ListVal>();
            if (pos == std::string::npos) {
                t->elems.push_back(rs.add(vm.makeString(right ? "" : s)));
                t->elems.push_back(rs.add(vm.makeString("")));
                t->elems.push_back(rs.add(vm.makeString(right ? s : "")));
            } else {
                t->elems.push_back(rs.add(vm.makeString(s.substr(0, pos))));
                t->elems.push_back(rs.add(vm.makeString(sep)));
                t->elems.push_back(rs.add(vm.makeString(s.substr(pos + sep.size()))));
            }
            return vm.alloc(std::move(t));
        });
    }
    // levenshtein(other): the Unicode (code-point) edit distance to another String -> Integer, or to
    // EACH String in a List -> a List of Integers (the source is decoded once, reused per candidate).
    if (name == "levenshtein")
        return bindReq("levenshtein", 1, {"other"}, [self, recv](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::vector<uint32_t> src = strCodepoints(recv(vm, self));
            const Object& arg = vm.arena().deref(a[0]);
            if (arg.kind() == ValueKind::String)
                return vm.makeInt(levenshteinDistance(src, strCodepoints(static_cast<const StrVal&>(arg).value())));
            if (arg.kind() == ValueKind::List) {
                RootScope rs(vm);
                auto out = std::make_unique<ListVal>();
                for (Handle e : static_cast<const ListVal&>(arg).elems) {
                    const Object& eo = vm.arena().deref(e);
                    if (eo.kind() != ValueKind::String)
                        throw KiritoError("levenshtein: the List must contain only Strings");
                    out->elems.push_back(rs.add(vm.makeInt(
                        levenshteinDistance(src, strCodepoints(static_cast<const StrVal&>(eo).value())))));
                }
                return vm.alloc(std::move(out));
            }
            throw KiritoError("levenshtein expects a String or a List of Strings");
        });
    return Object::getAttr(vm, self, name);
}

inline std::vector<std::string> StrVal::inspectMembers() const {
    return {
        "apply(fn) -> String", "center(width, fillchar) -> String",
        "count(sub, start, end) -> Integer", "encode(encoding) -> Bytes",
        "endswith(suffix, start, end) -> Bool", "find(sub, start, end) -> Integer",
        "format(...) -> String", "index(sub, start, end) -> Integer",
        "isalnum() -> Bool", "isalpha() -> Bool", "isdigit() -> Bool", "islower() -> Bool",
        "isspace() -> Bool", "isupper() -> Bool", "join(iterable) -> String",
        "levenshtein(other) -> Integer|List", "ljust(width, fillchar) -> String", "lower() -> String",
        "lstrip(chars) -> String", "partition(sep) -> List", "removeprefix(prefix) -> String",
        "removesuffix(suffix) -> String", "replace(old, new, count) -> String",
        "rfind(sub, start, end) -> Integer", "rindex(sub, start, end) -> Integer",
        "rjust(width, fillchar) -> String", "rpartition(sep) -> List", "rstrip(chars) -> String",
        "split(sep, maxsplit) -> List", "startswith(prefix, start, end) -> Bool",
        "strip(chars) -> String", "upper() -> String", "zfill(width) -> String",
    };
}

// --- List slice / contains, Dict / Set contains ----------------------------------------------

inline Handle ListVal::slice(KiritoVM& vm, Handle s, Handle e, Handle st) {
    auto result = std::make_unique<ListVal>();
    for (int64_t i : sliceIndices(vm, static_cast<int64_t>(elems.size()), s, e, st))
        result->elems.push_back(elems[i]);  // existing handles, reachable from this list
    return vm.alloc(std::move(result));
}
// Value equality that respects a user class's `_eq_` (so `in`, `index`, `count`, `remove` on a List
// agree with the `==` operator), falling back to structural equality. Tries either operand's `_eq_`
// (matching `x == e or e == x`), so it works regardless of which side is the instance.
inline bool kiEquals(KiritoVM& vm, Handle a, Handle b) {
    auto viaEq = [&](Handle x, Handle y) -> std::optional<bool> {
        Object& o = vm.arena().deref(x);
        if (o.kind() == ValueKind::Instance)
            if (auto* inst = dynamic_cast<InstanceValue*>(&o); inst && inst->findMethod(vm.arena(), "_eq_"))
                return vm.arena().deref(o.binary(vm, BinOp::Eq, x, y)).truthy();
        return std::nullopt;
    };
    if (auto r = viaEq(a, b)) return *r;
    if (auto r = viaEq(b, a)) return *r;
    Object& oa = vm.arena().deref(a);
    Object& ob = vm.arena().deref(b);
    // Containers compare element-wise with VM-aware equality, so a nested instance's _eq_ is honored
    // (the VM-less Object::equals would fall back to identity for instance elements/values). The
    // identity short-circuit applies only to containers — scalars must not (NaN != NaN).
    if (oa.kind() == ValueKind::List && ob.kind() == ValueKind::List) {
        if (&oa == &ob) return true;                         // same list object (cycle/self-reference)
        auto& la = static_cast<ListVal&>(oa);
        auto& lb = static_cast<ListVal&>(ob);
        if (la.elems.size() != lb.elems.size()) return false;
        EqualsGuard guard;                                   // cyclic-structure depth guard
        for (std::size_t i = 0; i < la.elems.size(); ++i)
            if (!kiEquals(vm, la.elems[i], lb.elems[i])) return false;
        return true;
    }
    if (oa.kind() == ValueKind::Dict && ob.kind() == ValueKind::Dict) {
        if (&oa == &ob) return true;                         // same dict object (cycle/self-reference)
        auto& da = static_cast<DictVal&>(oa);
        auto& db = static_cast<DictVal&>(ob);
        auto pa = da.pairs();
        if (pa.size() != db.pairs().size()) return false;
        EqualsGuard guard;
        for (const auto& [k, v] : pa) {                      // keys are hashable scalars -> structural lookup
            const Handle* vb = db.find(vm.arena(), k);
            if (!vb || !kiEquals(vm, v, *vb)) return false;
        }
        return true;
    }
    // Cross-type equality must stay symmetric: if oa.equals(ob) says no but ob would recognize oa
    // (e.g. 2 == Complex(2, 0): the Integer doesn't know about Complex, but the Complex does), give
    // the other side a chance. Skip when kinds match — same-kind dispatch is already symmetric.
    if (oa.equals(vm.arena(), ob)) return true;
    if (oa.kind() != ob.kind()) return ob.equals(vm.arena(), oa);
    return false;
}

inline bool ListVal::contains(KiritoVM& vm, Handle value) {
    // Search a GC-rooted snapshot: kiEquals may run a user _eq_ that reallocs/mutates this List, which
    // would dangle a live iterator over `elems` (A09-1); rooting keeps the handles alive if a reentrant
    // clear()+GC drops them.
    RootScope rs(vm);
    std::vector<Handle> snap = elems;
    for (Handle h : snap) rs.add(h);
    for (Handle h : snap)
        if (kiEquals(vm, h, value)) return true;
    return false;
}
inline bool DictVal::contains(KiritoVM& vm, Handle key) {
    return find(vm.arena(), key) != nullptr;
}
inline bool SetVal::contains(KiritoVM& vm, Handle value) {
    return contains(vm.arena(), value);
}

// --- Classes & instances ---------------------------------------------------------------------

inline Handle ClassValue::call(KiritoVM& vm, std::span<const Handle> args) {
    return callFull(vm, args, {});
}

inline Handle ClassValue::callFull(KiritoVM& vm, std::span<const Handle> args,
                                   std::span<const NamedArg> named) {
    auto inst = std::make_unique<InstanceValue>();
    inst->cls = selfHandle;
    inst->className = name;
    // Cache the dunder-availability flags now, walking the class chain once. Dict/Set hot paths
    // then read a plain bool instead of doing a method lookup per hash/equals.
    inst->hasHashDunder = findMethod(vm.arena(), "_hash_") != nullptr;
    inst->hasEqDunder   = findMethod(vm.arena(), "_eq_")   != nullptr;
    inst->hasBoolDunder = findMethod(vm.arena(), "_bool_") != nullptr;
    Handle instH = vm.alloc(std::move(inst));
    static_cast<InstanceValue&>(vm.arena().deref(instH)).selfHandle = instH;
    if (const Handle* init = findMethod(vm.arena(), "_init_")) {
        RootScope rs(vm);
        rs.add(instH);
        Handle initH = rs.add(*init);
        std::vector<Handle> full;
        full.reserve(args.size() + 1);
        full.push_back(instH);
        for (Handle a : args) full.push_back(a);
        Object& initObj = vm.arena().deref(initH);
        // A Kirito `_init_` binds keyword args; a native `_init_` (rare) takes positional only.
        if (initObj.kind() == ValueKind::Function)
            static_cast<KiFunction&>(initObj).callFull(vm, full, named);
        else
            initObj.call(vm, full);
    } else if (!named.empty() || !args.empty()) {
        throw KiritoError(name + "() takes no arguments (no _init_ defined)");
    }
    return instH;
}

// Build a bound method: a kwarg-aware callable that prepends `receiver` to the args, then invokes
// `methodH` (a Kirito function via callFull with kwargs, or a native via positional call). Shared by
// instance and `_super_` attribute lookup so the receiver-binding protocol lives in exactly one place.
inline Handle makeBoundMethod(KiritoVM& vm, std::string name, Handle receiver, Handle methodH) {
    return vm.alloc(std::make_unique<NativeFunction>(
        std::move(name),
        NativeFnKw{[receiver, methodH](KiritoVM& vm, std::span<const Handle> args,
                                       std::span<const NamedArg> named) -> Handle {
            std::vector<Handle> full;
            full.reserve(args.size() + 1);
            full.push_back(receiver);
            for (Handle a : args) full.push_back(a);
            Object& m = vm.arena().deref(methodH);
            if (m.kind() == ValueKind::Function)
                return static_cast<KiFunction&>(m).callFull(vm, full, named);
            return m.call(vm, full);  // native method: positional only
        }},
        std::vector<Handle>{receiver, methodH}));
}

inline Handle InstanceValue::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    auto it = attrs.find(std::string(name));
    if (it != attrs.end()) return it->second;
    const auto& klass = static_cast<const ClassValue&>(vm.arena().deref(cls));
    const Handle* method = klass.findMethod(vm.arena(), std::string(name));
    if (!method)
        throw KiritoError("'" + className + "' object has no attribute '" + std::string(name) + "'");
    Handle methodH = *method;
    // A non-callable class member (`var n = 5` in the class body) is a shared class attribute:
    // return the value itself. Only functions bind as methods (receiver prepended).
    {
        ValueKind mk = vm.arena().deref(methodH).kind();
        if (mk != ValueKind::Function && mk != ValueKind::NativeFunction) return methodH;
    }
    // Return a bound method: a callable that prepends the receiver before invoking the function. It
    // is kwarg-aware so `obj.method(x, k = 1)` forwards keyword arguments to the underlying Kirito
    // function — method calls accept keywords exactly like plain function calls.
    return makeBoundMethod(vm, std::string(name), self, methodH);
}

// Attribute writes on an instance / a module: barriered against the arena of the VM doing the write,
// which is why they live here rather than inline in class_value.hpp / module.hpp (KiritoVM is
// incomplete there). A module is a per-VM singleton that outlives the values bound into it, so once
// promoted it is a permanent old->young store site.
inline void InstanceValue::setAttr(KiritoVM& vm, std::string_view name, Handle value) {
    gcWriteBarrier(vm.arena(), this, value);
    attrs[std::string(name)] = value;
}
inline void ModuleValue::setAttr(KiritoVM& vm, std::string_view name, Handle value) {
    gcWriteBarrier(vm.arena(), this, value);
    members[std::string(name)] = value;
}

inline Handle SuperValue::getAttr(KiritoVM& vm, Handle, std::string_view name) {
    // Resolve the named method starting at startClass (the base of the current method's class), then
    // bind it to the ORIGINAL instance so the inherited method runs against the real self.
    const auto& base = static_cast<const ClassValue&>(vm.arena().deref(startClass));
    const Handle* method = base.findMethod(vm.arena(), std::string(name));
    if (!method)
        throw KiritoError("'super' object has no attribute '" + std::string(name) + "'");
    Handle methodH = *method;
    {
        // Same rule as instance lookup: a non-callable member is a plain value, not a method.
        ValueKind mk = vm.arena().deref(methodH).kind();
        if (mk != ValueKind::Function && mk != ValueKind::NativeFunction) return methodH;
    }
    return makeBoundMethod(vm, std::string(name), instance, methodH);
}

// --- `_hash_` / `_eq_` / `_bool_` opt-in on user classes ---------------------------------------
// These slots are const with no VM arg (Dict/Set call them deep, and every `if`/`while`/`and`/
// `or`/`not`/`Bool(x)` reaches `truthy()`), so the Kirito method is dispatched through
// KiritoVM::activeVM() — the thread's current VM. The instance stashes a bool at instantiation
// time so the hot path checks a plain field, not a hash-table lookup.

// `_bool_(self) -> Bool` — user-defined truthiness. Every conditional expression that reaches an
// instance eventually calls truthy(); if the class defines `_bool_`, call it and require a Bool
// return. Without `_bool_`, an instance is always truthy (the historical, additive-safe default).
inline bool InstanceValue::truthy() const {
    if (!hasBoolDunder) return true;
    KiritoVM* vm = KiritoVM::activeVM();
    if (!vm)
        throw KiritoError("_bool_ requires an active interpreter context");
    const Handle* m = findMethod(vm->arena(), "_bool_");
    if (!m) return true;   // defensive: the cache flag says it exists, but a rebound class could differ
    RootScope rs(*vm);
    Handle sh = rs.add(selfHandle);
    Handle mh = rs.add(*m);
    std::vector<Handle> args{sh};
    Handle out = vm->arena().deref(mh).call(*vm, args);
    const Object& r = vm->arena().deref(out);
    if (r.kind() != ValueKind::Bool)
        throw KiritoError("'" + className + "'._bool_ must return a Bool, got '" + r.typeName() + "'");
    return static_cast<const BoolVal&>(r).value();
}

inline std::size_t InstanceValue::hash() const {
    KiritoVM* vm = KiritoVM::activeVM();
    if (!vm)
        throw KiritoError("_hash_ requires an active interpreter context");
    const Handle* m = findMethod(vm->arena(), "_hash_");
    if (!m)
        throw KiritoError("unhashable type '" + className + "'");
    // Root the receiver + method through the call; a nested collection during the callee could
    // otherwise sweep them out from under us.
    RootScope rs(*vm);
    Handle sh = rs.add(selfHandle);
    Handle mh = rs.add(*m);
    std::vector<Handle> args{sh};
    Handle out = vm->arena().deref(mh).call(*vm, args);
    const Object& r = vm->arena().deref(out);
    if (r.kind() != ValueKind::Integer)
        throw KiritoError("'" + className + "'._hash_ must return an Integer, got '" + r.typeName() + "'");
    // Fold the signed Integer into size_t: negative values wrap via reinterpret; Dict/Set only need
    // "equal objects yield the same bucket", not any specific numeric identity.
    return static_cast<std::size_t>(static_cast<uint64_t>(static_cast<const IntVal&>(r).value()));
}

// Structural equality on user instances remains IDENTITY by default — deliberately, so that adding
// `_hash_` to a class is a strictly-additive change (an existing Dict/Set that stored instances
// keeps its lookup semantics). If a class ALSO defines `_eq_`, we consult it here so the invariant
// "`a == b` → hash(a) == hash(b)" holds inside Dict/Set — a `_hash_` implementation that returns
// the same value for equal-by-`_eq_` objects then makes Dict/Set behave value-based.
inline bool InstanceValue::equals(const ObjectArena& arena, const Object& other) const {
    if (this == &other) return true;
    if (!hasEqDunder) return false;
    // Every NativeClass (DateTime/Bytes/Matrix/…) ALSO reports ValueKind::Instance, so a kind check
    // is not enough — a raw downcast of a native object to InstanceValue would read a garbage Handle
    // (UB, reachable when a user instance and a native value share a Dict/Set bucket). dynamic_cast
    // returns null for anything that isn't actually an InstanceValue.
    const auto* rhsp = dynamic_cast<const InstanceValue*>(&other);
    if (!rhsp) return false;
    // We have `other` as a reference; the InstanceValue keeps its own `selfHandle` so we can pass
    // both sides to the Kirito `_eq_` method by re-using their cached handles.
    const auto& rhs = *rhsp;
    KiritoVM* vm = KiritoVM::activeVM();
    if (!vm) return false;
    const Handle* m = findMethod(arena, "_eq_");
    if (!m) return false;
    RootScope rs(*vm);
    Handle lh = rs.add(selfHandle);
    Handle rh = rs.add(rhs.selfHandle);
    Handle mh = rs.add(*m);
    std::vector<Handle> args{lh, rh};
    Handle outH = vm->arena().deref(mh).call(*vm, args);
    return vm->arena().deref(outH).truthy();
}

// Invoke a class method named `method` with [self, args...]; throws `notFound` if the class chain
// lacks it. Used by the operator-protocol slots below.
inline Handle invokeOp(KiritoVM& vm, InstanceValue& inst, const char* method,
                       std::span<const Handle> args, const std::string& notFound) {
    const Handle* m = inst.findMethod(vm.arena(), method);
    if (!m) throw KiritoError(notFound);
    RootScope rs(vm);
    Handle mh = rs.add(*m);
    std::vector<Handle> full;
    full.reserve(args.size() + 1);
    full.push_back(inst.selfHandle);
    for (Handle a : args) full.push_back(rs.add(a));
    return vm.arena().deref(mh).call(vm, full);
}

inline Handle InstanceValue::binary(KiritoVM& vm, BinOp op, Handle, Handle rhs) {
    std::array<Handle, 1> a{rhs};
    return invokeOp(vm, *this, binOpMethod(op), a,
                    "'" + className + "' has no operator '" + binOpMethod(op) + "'");
}
inline Handle InstanceValue::unary(KiritoVM& vm, UnOp op, Handle) {
    return invokeOp(vm, *this, unOpMethod(op), {},
                    "'" + className + "' has no operator '" + unOpMethod(op) + "'");
}
inline Handle InstanceValue::call(KiritoVM& vm, std::span<const Handle> args) {
    return invokeOp(vm, *this, "_call_", args, "'" + className + "' object is not callable");
}
inline Handle InstanceValue::callKw(KiritoVM& vm, std::span<const Handle> args,
                                    std::span<const NamedArg> named) {
    // Calling an instance dispatches to _call_, forwarding keyword arguments (kwargs everywhere).
    if (named.empty()) return call(vm, args);
    const Handle* m = findMethod(vm.arena(), "_call_");
    if (!m) throw KiritoError("'" + className + "' object is not callable");
    RootScope rs(vm);
    Handle mh = rs.add(*m);
    std::vector<Handle> full;
    full.reserve(args.size() + 1);
    full.push_back(selfHandle);
    for (Handle a : args) full.push_back(rs.add(a));
    Object& mo = vm.arena().deref(mh);
    if (mo.kind() == ValueKind::Function)
        return static_cast<KiFunction&>(mo).callFull(vm, full, named);
    throw KiritoError("'" + className + "' _call_ does not accept keyword arguments");
}
inline Handle InstanceValue::getItem(KiritoVM& vm, std::span<const Handle> keys) {
    return invokeOp(vm, *this, "_getitem_", keys, "'" + className + "' object is not indexable");
}
inline void InstanceValue::setItem(KiritoVM& vm, std::span<const Handle> keys, Handle value) {
    std::vector<Handle> args(keys.begin(), keys.end());
    args.push_back(value);
    invokeOp(vm, *this, "_setitem_", args, "'" + className + "' does not support item assignment");
}
inline std::optional<int64_t> InstanceValue::length(KiritoVM& vm) {
    Handle r = invokeOp(vm, *this, "_len_", {}, "'" + className + "' has no length");
    const Object& o = vm.arena().deref(r);
    if (o.kind() != ValueKind::Integer) throw KiritoError("_len_ must return an Integer");
    int64_t v = static_cast<const IntVal&>(o).value();
    if (v < 0) throw KiritoError("_len_ must return a non-negative Integer");  // a native consumer may cast to size_t
    return v;
}
inline bool InstanceValue::contains(KiritoVM& vm, Handle value) {
    std::array<Handle, 1> a{value};
    Handle r = invokeOp(vm, *this, "_contains_", a, "'" + className + "' does not support 'in'");
    return vm.arena().deref(r).truthy();
}

inline std::optional<std::vector<Handle>> InstanceValue::iterate(KiritoVM& vm) {
    // A class becomes iterable by defining _iter_(self) returning any iterable (commonly a List).
    if (!findMethod(vm.arena(), "_iter_")) return std::nullopt;
    // Guard against `_iter_` returning `self` (or a mutually-referential instance): the result is
    // re-dispatched through iterate(), and a self/cyclic return recurses in native C++ — each level's
    // `_iter_` call keeps the call-depth guard balanced, so it never trips → native stack overflow
    // (A07-1). Bound the re-dispatch depth and throw a catchable error; a legitimate `_iter_` chain
    // (A -> B -> List) is only a few levels deep. One OS thread == one VM, so thread_local is VM-scoped.
    static thread_local int iterDepth = 0;
    if (iterDepth >= 100)
        throw KiritoError("'" + className + "' _iter_ recurses too deeply (does _iter_ return self or a cycle?)");
    ++iterDepth;
    struct DepthGuard { int& d; ~DepthGuard() { --d; } } depthGuard{iterDepth};
    Handle r = invokeOp(vm, *this, "_iter_", {}, "'" + className + "' is not iterable");
    RootScope rs(vm);
    rs.add(r);
    return vm.arena().deref(r).iterate(vm);
}
inline std::string InstanceValue::str(StringifyCtx& ctx) const {
    if (ctx.vm) {
        const Handle* m = findMethod(ctx.arena, "_str_");
        if (m) {
            if (ctx.active.count(this)) return "<" + className + " object>";  // cycle guard
            RootScope rs(*ctx.vm);
            std::array<Handle, 1> full{selfHandle};
            Handle r = rs.add(ctx.vm->arena().deref(*m).call(*ctx.vm, full));
            const Object& o = ctx.vm->arena().deref(r);
            if (o.kind() == ValueKind::String) return static_cast<const StrVal&>(o).value();
            // `_str_` returned a non-String (another object we must stringify). `ctx.active` only
            // catches a _str_ that returns SELF (or a back-reference); a chain of DISTINCT instances
            // (a._str_ -> b, b._str_ -> c, ...) never repeats, so bound ctx.depth like the container
            // stringifier does, else deep native recursion overflows the C++ stack (a hard crash).
            if (++ctx.depth > 1000) {
                --ctx.depth;
                throw KiritoError("'" + className + "' _str_ recurses too deeply (does _str_ return "
                                  "self or a cycle of objects?)");
            }
            ctx.active.insert(this);
            std::string s = o.str(ctx);
            ctx.active.erase(this);
            --ctx.depth;
            return s;
        }
    }
    return "<" + className + " object>";
}

// --- KiFunction call -------------------------------------------------------------------------

// The canonical type-name a value matches for annotation checks. Built-ins map to their kind;
// user instances report their class name (and inheritance is handled separately by typeMatches).
inline std::string annotationTypeName(ValueKind k) {
    switch (k) {
        case ValueKind::None: { return "None"; } break;
        case ValueKind::Bool: { return "Bool"; } break;
        case ValueKind::Integer: { return "Integer"; } break;
        case ValueKind::Float: { return "Float"; } break;
        case ValueKind::String: { return "String"; } break;
        case ValueKind::List: { return "List"; } break;
        case ValueKind::Dict: { return "Dict"; } break;
        case ValueKind::Set: { return "Set"; } break;
        case ValueKind::Function: case ValueKind::NativeFunction: { return "Function"; } break;
        case ValueKind::Module: { return "Module"; } break;
        case ValueKind::Class: { return "Class"; } break;
        default: { return ""; } break;
    }
}

// Does `value` satisfy the type annotation `typeName`? Built-in type names match by kind; "Any"
// always matches; otherwise treat it as a class name and check the instance's class chain (so
// subclasses pass) — and also accept a NativeClass whose typeName equals the annotation.
inline bool typeMatches(KiritoVM& vm, Handle value, const std::string& typeName) {
    if (typeName.empty() || typeName == "Any") return true;
    const Object& o = vm.arena().deref(value);
    // "Number" is the pseudo-type the numeric builtins advertise (inspect shows `x: Number`); accept
    // it for Integer or Float so a user annotation `Function(x : Number)` works as documented.
    if (typeName == "Number") return o.kind() == ValueKind::Integer || o.kind() == ValueKind::Float;
    // built-in kind names
    if (annotationTypeName(o.kind()) == typeName) return true;
    // a user instance: walk its class chain by name (inheritance-aware)
    if (o.kind() == ValueKind::Instance) {
        const auto* inst = dynamic_cast<const InstanceValue*>(&o);
        if (inst) {
            Handle cur = inst->cls;
            while (true) {
                const auto& c = static_cast<const ClassValue&>(vm.arena().deref(cur));
                if (c.name == typeName) return true;
                if (!c.hasBase) break;
                cur = c.base;
            }
        }
        // a C++ NativeClass instance (Matrix, Socket, ...): match its own type name
        if (o.typeName() == typeName) return true;
    }
    return false;
}

// Resolve a type argument (for `isinstance` and typed `catch`) to a type name: a Class -> its name,
// a String -> its text, a built-in type constructor (the NativeFunctions named Integer/Float/String/
// Bool/Bytes/List/Set/Dict) -> that name. Returns "" if the value can't act as a type. This is what
// lets `isinstance(1, Integer)` and `catch String` work, not just the String-name forms.
inline std::string resolveTypeName(KiritoVM& vm, Handle typeH) {
    const Object& t = vm.arena().deref(typeH);
    if (t.kind() == ValueKind::Class) return static_cast<const ClassValue&>(t).name;
    if (t.kind() == ValueKind::String) return static_cast<const StrVal&>(t).value();
    if (t.kind() == ValueKind::NativeFunction) {
        const std::string& n = static_cast<const NativeFunction&>(t).name();
        if (n == "Integer" || n == "Float" || n == "String" || n == "Bool" ||
            n == "Bytes" || n == "List" || n == "Set" || n == "Dict")
            return n;
    }
    return "";
}

inline Handle KiFunction::call(KiritoVM& vm, std::span<const Handle> args) {
    return callFull(vm, args, {});
}

inline Handle KiFunction::callFull(KiritoVM& vm, std::span<const Handle> positional,
                                   std::span<const NamedArg> named) {
    CallGuard depth(vm);  // bound native-stack recursion -> catchable error, not a crash
    // While this body runs, the "current chunk" is THIS function's defining file, so any closures it
    // creates at runtime (e.g. functools.partial's returned function) inherit the right source file
    // for error attribution — not whatever script happened to call us.
    std::optional<KiritoVM::ChunkFileScope> chunkScope;
    if (!sourceFile.empty()) chunkScope.emplace(vm, sourceFile);
    const auto& params = def_->params;
    RootScope rs(vm);
    Handle scope = rs.add(vm.newScope(closure_));
    auto& env = static_cast<EnvValue&>(vm.arena().deref(scope));
    env.reserve(params.size());

    // Fast path: the overwhelmingly common call shape — only positional args, exact arity, no
    // type annotations to check. Bind straight into the scope with no temporaries.
    if (!def_->fastBindable.has_value()) {
        bool simple = def_->returnAnnotation.empty();
        for (const auto& p : params)
            if (!p.annotation.empty()) { simple = false; break; }
        def_->fastBindable = simple;  // memoize the per-def annotation check (def_ field is mutable)
    }
    // Attribute an error escaping the body to this function's defining chunk (file or frozen
    // module) — the body's spans refer to that source, not to whichever script called it.
    auto attributed = [this](auto&& run) -> Handle {
        try {
            return run();
        } catch (KiritoError& e) {
            if (e.file.empty() && !sourceFile.empty()) e.file = sourceFile;
            throw;
        } catch (KiritoThrow& t) {
            if (t.file.empty() && !sourceFile.empty()) t.file = sourceFile;
            throw;
        }
    };

    // Compile (once) and execute the body on the bytecode engine — the sole execution path.
    auto runBody = [&](Handle bodyScope, std::span<const Handle> paramValues) -> Handle {
        return runBytecodeBody(vm, bodyScope, def_->body, hasOwner ? ownerClass : Handle{}, hasOwner,
                               /*isFunction=*/true, def_->name.empty() ? "<function>" : def_->name, def_,
                               paramValues);
    };

    if (named.empty() && positional.size() == params.size() && *def_->fastBindable) {
        for (std::size_t i = 0; i < params.size(); ++i) env.define(vm.arena(), params[i].name, positional[i]);
        return attributed([&] { return runBody(scope, positional); });
    }

    std::vector<bool> bound(params.size(), false);
    std::vector<Handle> values(params.size());

    if (positional.size() > params.size())
        throw KiritoError("function takes " + std::to_string(params.size()) + " positional argument(s) but " +
                          std::to_string(positional.size()) + " were given");
    for (std::size_t i = 0; i < positional.size(); ++i) { values[i] = positional[i]; bound[i] = true; }

    // named args: match each to a parameter by name
    for (const auto& na : named) {
        std::size_t idx = params.size();
        for (std::size_t i = 0; i < params.size(); ++i)
            if (params[i].name == na.name) { idx = i; break; }
        if (idx == params.size())
            throw KiritoError("function got an unexpected keyword argument '" + na.name + "'");
        if (bound[idx])
            throw KiritoError("function got multiple values for argument '" + na.name + "'");
        values[idx] = na.value;
        bound[idx] = true;
    }

    // fill defaults / report missing, then enforce annotations
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (!bound[i]) {
            if (params[i].defaultValue) {
                // defaults evaluate in the call scope (closure-visible), once per call
                values[i] = rs.add(runBytecodeExpr(vm, scope, *params[i].defaultValue));
            } else {
                throw KiritoError("function missing required argument '" + params[i].name + "'");
            }
        }
        if (!params[i].annotation.empty() && !typeMatches(vm, values[i], params[i].annotation))
            throw KiritoError("argument '" + params[i].name + "' must be " + params[i].annotation +
                              ", got " + vm.arena().deref(values[i]).typeName());
        env.define(vm.arena(), params[i].name, values[i]);
    }

    Handle result = attributed([&] { return runBody(scope, values); });

    // enforce the return annotation
    if (!def_->returnAnnotation.empty() && !typeMatches(vm, result, def_->returnAnnotation))
        throw KiritoError("function must return " + def_->returnAnnotation + ", got " +
                          vm.arena().deref(result).typeName());
    return result;
}

// --- operation semantics (the single source of truth, used by the bytecode VM) ----------------
//
// These free functions are the single source of truth for operator/call/member dispatch; the
// BytecodeVM's opcode handlers call them and add only their own span-tagging.

// True if `value` is an instance of class `typeH` (walking the base chain). Only user-defined
// InstanceValues carry a class handle; native C++ objects report ValueKind::Instance too, so a
// dynamic_cast tells them apart.
inline bool isInstanceOf(KiritoVM& vm, Handle value, Handle typeH) {
    if (vm.arena().deref(typeH).kind() == ValueKind::Class) {
        const Object& v = vm.arena().deref(value);
        const auto* inst = dynamic_cast<const InstanceValue*>(&v);
        if (!inst) return false;
        Handle cur = inst->cls;
        while (true) {
            if (cur == typeH) return true;
            const auto& c = static_cast<const ClassValue&>(vm.arena().deref(cur));
            if (!c.hasBase) return false;
            cur = c.base;
        }
    }
    // Not a user class: a built-in type constructor or a type-name String (catch Integer / catch
    // String / catch "RuntimeError"-style names) — match by name (kind names + NativeClass typeName).
    std::string name = resolveTypeName(vm, typeH);
    return !name.empty() && typeMatches(vm, value, name);
}

// True if class `subH` is `superH` or descends from it (walking the base chain). Both handles must
// name user classes.
inline bool classIsSubclassOf(KiritoVM& vm, Handle subH, Handle superH) {
    Handle cur = subH;
    while (true) {
        if (cur == superH) return true;
        const auto& c = static_cast<const ClassValue&>(vm.arena().deref(cur));
        if (!c.hasBase) return false;
        cur = c.base;
    }
}

// A private member (_name, no trailing underscore) may only be touched from within a method whose
// class is in the receiver's class chain — privacy is per class *chain*, not per defining class, so
// a subclass method may read a base instance's private and vice versa (CLAUDE.md). currentClass/
// hasCurrentClass describe the method currently executing; access is allowed when the running
// method's class and the receiver's class are related by inheritance in EITHER direction.
inline void checkPrivateAccess(KiritoVM& vm, Handle obj, const std::string& name, Handle currentClass,
                               bool hasCurrentClass, SourceSpan span) {
    if (!isPrivateName(name)) return;
    const auto* inst = dynamic_cast<const InstanceValue*>(&vm.arena().deref(obj));
    if (!inst) return;  // user classes only
    if (hasCurrentClass && vm.arena().deref(currentClass).kind() == ValueKind::Class) {
        Handle objClass = inst->cls;
        if (classIsSubclassOf(vm, objClass, currentClass) ||
            classIsSubclassOf(vm, currentClass, objClass))
            return;
    }
    throw KiritoError("cannot access private member '" + name + "' of '" +
                      vm.arena().deref(obj).typeName() + "' outside its class", span);
}

inline Handle applyUnaryOp(KiritoVM& vm, UnOp op, Handle operand) {
    if (op == UnOp::Not) {
        // An instance may override `not` via _not_; otherwise negate truthiness. Like `_neg_` (and
        // unlike `_bool_`), `_not_`'s return value is NOT coerced to a Bool — `not <instance>` yields
        // the raw _not_ result (a documented, tested behaviour: r7_types.ki "returns the raw value").
        Object& o = vm.arena().deref(operand);
        if (o.kind() == ValueKind::Instance && dynamic_cast<InstanceValue*>(&o) &&
            static_cast<InstanceValue&>(o).findMethod(vm.arena(), "_not_"))
            return o.unary(vm, op, operand);
        return vm.makeBool(!vm.arena().deref(operand).truthy());
    }
    return vm.arena().deref(operand).unary(vm, op, operand);
}

inline Handle applyBinaryOp(KiritoVM& vm, BinOp op, Handle lhs, Handle rhs) {
    // Fast path: numeric arithmetic/ordering (Integer/Float, any mix) goes straight to the shared
    // numericBinary primitives, skipping the Eq/Ne/In branch checks and the virtual binary() call on
    // the hottest operator path. Exactly equivalent — IntVal/FloatVal::binary delegate here anyway,
    // with identical wraparound / true-division `/` / throw-on-zero-divisor / exact-compare semantics.
    // Eq/Ne (exact structural) and In/NotIn keep their dedicated handling; `Integer * sequence` is
    // naturally excluded since a sequence operand is non-numeric.
    if (op != BinOp::Eq && op != BinOp::Ne && op != BinOp::In && op != BinOp::NotIn) {
        const Object& l = vm.arena().deref(lhs);
        if (isNumeric(l) && isNumeric(vm.arena().deref(rhs))) return numericBinary(vm, op, lhs, rhs);
    }
    // Equality never throws on a type mismatch (1 == "x" is False); it uses structural equals, unless
    // a user class overrides it. Ordering/arithmetic dispatch through binary(); in/not in via contains.
    if (op == BinOp::Eq || op == BinOp::Ne) {
        Object& l = vm.arena().deref(lhs);
        if (l.kind() == ValueKind::Instance) {
            if (auto* inst = dynamic_cast<InstanceValue*>(&l)) {
                if (inst->findMethod(vm.arena(), binOpMethod(op)))
                    return l.binary(vm, op, lhs, rhs);
                if (op == BinOp::Ne && inst->findMethod(vm.arena(), "_eq_")) {
                    Handle eqr = l.binary(vm, BinOp::Eq, lhs, rhs);
                    return vm.makeBool(!vm.arena().deref(eqr).truthy());
                }
            }
        }
        // Reflected: the LEFT operand is not an instance but the RIGHT is (`5 != c`). Prefer the
        // RIGHT instance's own `_ne_`/`_eq_` so it is symmetric with `c != 5`, instead of silently
        // falling through to structural equality (which ignores a standalone `_ne_`). ==/!= are
        // symmetric, so the operand order can be swapped.
        if (l.kind() != ValueKind::Instance) {
            Object& r = vm.arena().deref(rhs);
            if (auto* rinst = dynamic_cast<InstanceValue*>(&r)) {
                if (rinst->findMethod(vm.arena(), binOpMethod(op)))
                    return r.binary(vm, op, rhs, lhs);
                if (op == BinOp::Ne && rinst->findMethod(vm.arena(), "_eq_")) {
                    Handle eqr = r.binary(vm, BinOp::Eq, rhs, lhs);
                    return vm.makeBool(!vm.arena().deref(eqr).truthy());
                }
            }
        }
        bool eq = kiEquals(vm, lhs, rhs);
        return vm.makeBool(op == BinOp::Eq ? eq : !eq);
    }
    if (op == BinOp::In || op == BinOp::NotIn) {
        bool c = vm.arena().deref(rhs).contains(vm, lhs);
        return vm.makeBool(op == BinOp::In ? c : !c);
    }
    // NOTE: arithmetic/ordering operators deliberately do NOT reflect onto a right-hand object
    // (`3 * v` throws even if `v` defines `_mul_` — the object must be on the left). This is a
    // documented invariant (tools/tests/scripts/r11_docinvariants.ki) — Kirito has no `_radd_`-style
    // reflected dunder, so a correct reflection is impossible for the non-commutative ops. Only
    // ==/!= are symmetric (handled above).
    return vm.arena().deref(lhs).binary(vm, op, lhs, rhs);
}

// Dispatch a call with already-evaluated positional + named arguments to any callable: a Kirito
// function, a class (instantiation forwards kwargs to _init_), a native function (signatured/kwarg/
// exact-arity), or an instance (_call_). The caller wraps this with its own location tag.
inline Handle applyCall(KiritoVM& vm, Handle callee, std::span<const Handle> positional,
                        std::span<const NamedArg> named) {
    Object& c = vm.arena().deref(callee);
    const ValueKind k = c.kind();   // hoisted: this is the hot call path, kind() is virtual
    if (named.empty()) {
        if (k == ValueKind::Function)
            return static_cast<KiFunction&>(c).callFull(vm, positional, {});
        if (k == ValueKind::NativeFunction && static_cast<NativeFunction&>(c).hasSignature() &&
            positional.size() != static_cast<NativeFunction&>(c).params().size()) {
            auto& nf = static_cast<NativeFunction&>(c);
            RootScope rs(vm);
            std::vector<Handle> bound = nf.bindArgs(positional, {});
            for (Handle h : bound) rs.add(h);
            return nf.call(vm, bound);
        }
        return c.call(vm, positional);
    }
    if (k == ValueKind::Function)
        return static_cast<KiFunction&>(c).callFull(vm, positional, named);
    if (k == ValueKind::Class)
        return static_cast<ClassValue&>(c).callFull(vm, positional, named);
    if (k == ValueKind::NativeFunction && static_cast<NativeFunction&>(c).acceptsKwargs())
        return static_cast<NativeFunction&>(c).callKw(vm, positional, named);
    if (k == ValueKind::NativeFunction && static_cast<NativeFunction&>(c).hasSignature()) {
        auto& nf = static_cast<NativeFunction&>(c);
        RootScope rs(vm);
        std::vector<Handle> bound = nf.bindArgs(positional, named);
        for (Handle h : bound) rs.add(h);
        return nf.call(vm, bound);
    }
    if (k == ValueKind::Instance && dynamic_cast<InstanceValue*>(&c))
        return static_cast<InstanceValue&>(c).callKw(vm, positional, named);
    throw KiritoError("this callable does not accept keyword arguments");
}

// Read obj.name with full member semantics: the self._super_() builder (inside a method of an
// inheriting class), the private-access check, then the value protocol's getAttr.
inline Handle evalMemberGet(KiritoVM& vm, Handle obj, const std::string& name, Handle currentClass,
                            bool hasCurrentClass, SourceSpan span) {
    if (name == "_super_" && hasCurrentClass) {
        Object& o = vm.arena().deref(obj);
        if (o.kind() == ValueKind::Instance && dynamic_cast<InstanceValue*>(&o)) {
            const auto& ownerCls = static_cast<const ClassValue&>(vm.arena().deref(currentClass));
            if (!ownerCls.findMethod(vm.arena(), "_super_")) {  // not overridden
                Handle objH = obj, ownerH = currentClass;
                return vm.alloc(std::make_unique<NativeFunction>(
                    "_super_",
                    [objH, ownerH](KiritoVM& v, std::span<const Handle>) -> Handle {
                        const auto& oc = static_cast<const ClassValue&>(v.arena().deref(ownerH));
                        if (!oc.hasBase)
                            throw KiritoError("_super_() called in '" + oc.name +
                                              "', which does not inherit from any class");
                        auto sup = std::make_unique<SuperValue>();
                        sup->instance = objH;
                        sup->startClass = oc.base;
                        return v.alloc(std::move(sup));
                    },
                    std::vector<Handle>{objH, ownerH}));
            }
        }
    }
    checkPrivateAccess(vm, obj, name, currentClass, hasCurrentClass, span);
    return vm.arena().deref(obj).getAttr(vm, obj, name);
}

// Spread an iterable `value` across `n` unpack slots, with an optional starred slot at `starIndex`
// (-1 if none) that absorbs the surplus into a List. Returns the n slot values (the caller must root
// them). Used by the bytecode engine's Unpack opcode (var/for/tuple-assign destructuring).
inline std::vector<Handle> spreadValues(KiritoVM& vm, Handle value, std::size_t n, int starIndex,
                                        SourceSpan span) {
    std::optional<std::vector<Handle>> items;
    try {
        items = vm.arena().deref(value).iterate(vm);
    } catch (KiritoError& err) {
        if (err.span.line == 0) err.span = span;
        throw;
    }
    if (!items)
        throw KiritoError("cannot unpack non-iterable '" + vm.arena().deref(value).typeName() + "'", span);
    RootScope rs(vm);  // keep iterated (possibly freshly-allocated) items alive during the alloc below
    for (Handle it : items.value()) rs.add(it);
    std::vector<Handle>& v = items.value();
    std::vector<Handle> slots(n);
    if (starIndex == -1) {
        if (v.size() != n)
            throw KiritoError("expected " + std::to_string(n) + " values to unpack, got " +
                              std::to_string(v.size()), span);
        for (std::size_t i = 0; i < n; ++i) slots[i] = v[i];
    } else {
        std::size_t before = static_cast<std::size_t>(starIndex), after = n - 1 - before;
        if (v.size() < before + after)
            throw KiritoError("expected at least " + std::to_string(before + after) +
                              " values to unpack, got " + std::to_string(v.size()), span);
        for (std::size_t i = 0; i < before; ++i) slots[i] = v[i];
        auto mid = std::make_unique<ListVal>();
        for (std::size_t i = before; i < v.size() - after; ++i) mid->elems.push_back(v[i]);
        slots[before] = vm.alloc(std::move(mid));
        for (std::size_t j = 0; j < after; ++j) slots[n - 1 - j] = v[v.size() - 1 - j];
    }
    return slots;
}

// Canonical type+value key for switch dispatch. Only hashable scalar kinds can label a case or be
// matched; other types yield nullopt (they only reach `default`). Matching is exact by type AND
// value, so `case 1` and `case 1.0` differ. Used by the bytecode SwitchMatch opcode.
inline std::optional<std::string> scalarSwitchKey(KiritoVM& vm, Handle h) {
    const Object& o = vm.arena().deref(h);
    switch (o.kind()) {
        case ValueKind::None: { return std::string("N"); } break;
        case ValueKind::Bool: { return std::string("B") + (static_cast<const BoolVal&>(o).value() ? "1" : "0"); } break;
        case ValueKind::Integer: { return "I" + std::to_string(static_cast<const IntVal&>(o).value()); } break;
        case ValueKind::Float: {
            double d = static_cast<const FloatVal&>(o).value();
            if (std::isnan(d)) return std::nullopt;            // NaN != NaN: matches no case (-> default)
            if (d == 0.0) d = 0.0;                             // 0.0 == -0.0: same key
            return "F" + floatToRoundtrip(d);                  // EXACT key (not 6-digit to_string), agrees with ==
        } break;
        case ValueKind::String: { return "S" + static_cast<const StrVal&>(o).value(); } break;
        default: { return std::nullopt; } break;
    }
}

// --- VM entry point & lifetime ---------------------------------------------------------------

// --- module / extension API ------------------------------------------------------------------

inline void KiritoVM::registerModule(std::string name, ModuleFactory factory) {
    moduleFactories_[std::move(name)] = std::move(factory);
}

template <class T>
void KiritoVM::install() {
    nativeModules_.push_back(std::make_unique<T>());
    NativeModule* mod = nativeModules_.back().get();
    registerModule(mod->name(), [mod](KiritoVM& vm) -> Handle {
        Handle h = vm.alloc(std::make_unique<ModuleValue>(mod->name()));
        RootScope rs(vm);  // keep the module alive while setup() allocates members (which may GC)
        rs.add(h);
        ModuleBuilder builder(vm, h, static_cast<ModuleValue&>(vm.arena().deref(h)));
        // No GC while a module installs itself. A member's DEFAULTS are written as arguments at the
        // call site — `m.fn("socket", {{"family", "String", vm.makeString("inet")}, {"type",
        // "String", vm.makeString("stream")}}, …)` — so they are unrooted temporaries until the
        // NativeFunction that traces them exists: allocating the second collects the first, and the
        // module ships a member whose default is a dangling handle (fires only when a caller omits
        // that argument). Rooting them inside ModuleBuilder::fn is TOO LATE — the damage is done
        // while the argument list is still being evaluated. Pausing here fixes every module and
        // every default at once, instead of asking dozens of call sites to remember. setup() runs
        // once per module and allocates a bounded amount, so nothing needs collecting during it.
        // This is reachable: install<T>() registers a FACTORY, so setup() runs lazily on first
        // import — long after an embedder may have set an aggressive threshold. (v1.15 A19-1.)
        GcPauseScope noGc(vm);
        mod->setup(builder);
        return h;
    });
}

inline void KiritoVM::registerSourceModule(std::string name, std::string_view source) {
    // A frozen module: the Kirito `source` is compiled and run in a fresh module scope on first
    // import, and its top-level bindings become the module's members. Cached like any module.
    std::string src(source);
    std::string modName = name;
    registerModule(std::move(name), [src, modName](KiritoVM& vm) -> Handle {
        try {
            Handle scope = vm.newModuleScope(/*isMain=*/false);  // a frozen module is imported -> argmain False, arglist empty (matches .ki-file imports)
            RootScope guard(vm);
            guard.add(scope);
            KiritoVM::ChunkFileScope chunkScope(vm, "<" + modName + ">");  // frozen-chunk attribution
            Lexer lex(src);
            auto toks = lex.tokenize();
            auto prog = std::make_unique<ast::Program>(Parser(std::move(toks), lex.source()).parseProgram());
            const ast::Program& program = *prog;
            vm.retainChunk(std::move(prog));
            Resolver(vm).resolve(program, scope, /*indexTopLevel=*/true);  // compile-time name resolution
            runBytecodeBody(vm, scope, program.stmts, Handle{}, /*hasOwner=*/false, /*isFunction=*/false);
            auto mod = std::make_unique<ModuleValue>(modName);
            for (const auto& [k, v] : static_cast<EnvValue&>(vm.arena().deref(scope)).locals())
                // hide private top-level names, the injected per-file env (arglist/argmain), and any
                // pre-declared slot that was never assigned (still the `undefined()` placeholder)
                if (!k.empty() && k.front() != '_' && k != "arglist" && k != "argmain" && v != vm.undefined())
                    mod->members[k] = v;
            return vm.alloc(std::move(mod));
        } catch (KiritoError& e) {
            // Attribute diagnostics to the frozen module, not the importing script (whose line
            // numbers would otherwise be combined with this chunk's positions).
            if (e.file.empty()) e.file = "<" + modName + ">";
            throw;
        } catch (KiritoThrow& t) {
            if (t.file.empty()) t.file = "<" + modName + ">";
            throw;
        }
    });
}

inline Handle KiritoVM::importModule(const std::string& name) {
    auto cached = moduleCache_.find(name);
    if (cached != moduleCache_.end()) return cached->second;  // modules are per-VM singletons

    // Circular-import detection. A module's members are published to moduleCache_ only AFTER its body
    // has fully evaluated, so if loading `name` (directly or transitively) asks to import `name`
    // again, the import chain has looped. Throw a clear diagnostic naming the cycle instead of
    // recursing until the native stack or the call-depth guard blows.
    auto cycleError = [&](const std::string& dup) {
        std::string chain;
        for (const auto& m : importStack_) chain += m + " -> ";
        return KiritoError("circular import detected: " + chain + dup);
    };
    if (importing_.count(name)) throw cycleError(name);

    // Bound the import-nesting DEPTH. importStack_ holds only the current in-progress chain (LoadGuard
    // pops on return), so a wide import graph never trips this; only an unbounded deep chain
    // (a imports b imports c ...) does, which would otherwise recurse importModule -> run -> import
    // until the native stack overflows (an uncatchable segfault). 500 is far beyond any real graph.
    if (importStack_.size() >= 500)
        throw KiritoError("maximum import nesting depth exceeded (deeply chained imports)");

    // RAII: mark `name` (and, for a .ki file, its resolved path) in-progress for the duration of the
    // load; always unwound on return/throw so a failed import never poisons a later one. References
    // are passed in so the local type needs no access to KiritoVM's private members.
    struct LoadGuard {
        fum::unordered_set<std::string>& importing;
        std::vector<std::string>& stack;
        std::vector<std::string> marked;
        LoadGuard(fum::unordered_set<std::string>& imp, std::vector<std::string>& st, const std::string& n)
            : importing(imp), stack(st) { stack.push_back(n); mark(n); }
        void mark(const std::string& k) { if (importing.insert(k).second) marked.push_back(k); }
        ~LoadGuard() { for (const auto& k : marked) importing.erase(k); stack.pop_back(); }
    } loadGuard(importing_, importStack_, name);

    auto factory = moduleFactories_.find(name);
    if (factory != moduleFactories_.end()) {
        Handle h = factory->second(*this);
        moduleCache_[name] = h;
        return h;
    }
    // Otherwise search the library paths for <name>.ki and load it as a module: the file's
    // top-level bindings become the module's members. The file is lexed+parsed+evaluated at most
    // once per VM — deduplicated by resolved absolute path, so the same file reached via different
    // module names (or repeated imports) reuses the one already-built module.
    // The name may be given with or without the `.ki` extension: import("io") and import("io.ki")
    // both resolve the file `io.ki` (a leading-or-trailing extension isn't doubled).
    std::string fileBase = name;
    if (fileBase.size() >= 3 && fileBase.compare(fileBase.size() - 3, 3, ".ki") == 0)
        fileBase = fileBase.substr(0, fileBase.size() - 3);
    for (const auto& dir : libPaths_) {
        std::filesystem::path path = std::filesystem::path(dir) / (fileBase + ".ki");
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        std::filesystem::path canon = std::filesystem::weakly_canonical(path, ec);
        std::string key = ec ? path.string() : canon.string();
        if (auto it = pathCache_.find(key); it != pathCache_.end()) {
            moduleCache_[name] = it->second;  // same file already loaded under another name
            return it->second;
        }
        // The same file reached mid-load under a different module name is still a cycle.
        if (importing_.count(key)) throw cycleError(key);
        loadGuard.mark(key);
        std::ifstream in(path);
        std::stringstream buf;
        buf << in.rdbuf();
        Handle scope = newModuleScope(/*isMain=*/false);  // imported -> argmain is False
        try {
            RootScope guard(*this);
            guard.add(scope);
            ChunkFileScope chunkScope(*this, path.string());  // functions defined here carry this file
            Lexer lex(buf.str());
            auto toks = lex.tokenize();
            auto prog = std::make_unique<ast::Program>(
                Parser(std::move(toks), lex.source()).parseProgram());
            const ast::Program& program = *prog;
            retainChunk(std::move(prog));
            programByFile_[path.string()] = &program;  // for parallel.spawn span lookup in workers
            Resolver(*this).resolve(program, scope, /*indexTopLevel=*/true);  // compile-time name resolution
            runBytecodeBody(*this, scope, program.stmts, Handle{}, /*hasOwner=*/false, /*isFunction=*/false);
            auto mod = std::make_unique<ModuleValue>(name);
            for (const auto& [k, v] : static_cast<EnvValue&>(arena_.deref(scope)).locals())
                if (k != "arglist" && k != "argmain" && v != undefined())  // exclude injected env + unwritten slots
                    mod->members[k] = v;
            Handle h = alloc(std::move(mod));
            moduleCache_[name] = h;
            pathCache_[key] = h;
            return h;
        } catch (KiritoError& e) {
            if (e.file.empty()) e.file = path.string();  // attribute the diagnostic to this module
            throw;
        } catch (KiritoThrow& t) {
            if (t.file.empty()) t.file = path.string();  // ditto for an uncaught `throw` at import
            throw;
        }
    }
    throw KiritoError("no module named '" + name + "'");
}

// --- introspection (`inspect` builtin) -------------------------------------------------------

// Render a function's signature from its AST: name(p1: T1, p2 = default, ...) -> Ret. Annotations
// and defaults are shown when present; `Any`/none are simply omitted.
inline std::string inspectSignature(const std::string& name, const ast::FunctionExpr& def) {
    std::string out = name + "(";
    for (std::size_t i = 0; i < def.params.size(); ++i) {
        if (i) out += ", ";
        out += def.params[i].name;
        if (!def.params[i].annotation.empty()) out += ": " + def.params[i].annotation;
        if (def.params[i].defaultValue) out += " = ...";
    }
    out += ")";
    if (!def.returnAnnotation.empty()) out += " -> " + def.returnAnnotation;
    return out;
}

// Render a native function's declared signature: name(p: T, q = <default>, ...) -> Ret. Defaults are
// shown as their actual value (the VM is on hand to stringify them).
inline std::string inspectNativeSignature(KiritoVM& vm, const std::string& name, const NativeFunction& nf) {
    std::string out = name + "(";
    const auto& ps = nf.params();
    for (std::size_t i = 0; i < ps.size(); ++i) {
        if (i) out += ", ";
        out += ps[i].name;
        if (!ps[i].annotation.empty()) out += ": " + ps[i].annotation;
        if (ps[i].hasDefault) {
            const Object& d = vm.arena().deref(ps[i].defaultValue);
            // Quote String defaults so they read unambiguously (mode = "r", not mode = r).
            out += " = " + (d.kind() == ValueKind::String ? "\"" + vm.stringify(ps[i].defaultValue) + "\""
                                                          : vm.stringify(ps[i].defaultValue));
        }
    }
    out += ")";
    if (!nf.returnType().empty()) out += " -> " + nf.returnType();
    return out;
}

// Human-readable introspection of any value: lists public methods/attributes (with signatures and
// type annotations where declared) for classes, instances, modules, and functions. Returns a String.
inline std::string inspectValue(KiritoVM& vm, Handle h) {
    const Object& o = vm.arena().deref(h);
    auto sortedKeys = [](const fum::unordered_map<std::string, Handle>& m) {
        std::vector<std::string> keys;
        for (const auto& [k, v] : m) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        return keys;
    };
    // Describe one member: a function shows its signature; anything else shows its type.
    auto describe = [&](const std::string& key, Handle mh, const char* indent) -> std::string {
        const Object& m = vm.arena().deref(mh);
        if (m.kind() == ValueKind::Function)
            return std::string(indent) + inspectSignature(key, static_cast<const KiFunction&>(m).def()) + "\n";
        if (m.kind() == ValueKind::NativeFunction) {
            const auto& nf = static_cast<const NativeFunction&>(m);
            if (nf.hasSignature())
                return std::string(indent) + inspectNativeSignature(vm, key, nf) + "  [native]\n";
            return std::string(indent) + key + "(...)  [native]\n";
        }
        return std::string(indent) + key + ": " + m.typeName() + "\n";
    };

    switch (o.kind()) {
        case ValueKind::Class: {
            const auto& c = static_cast<const ClassValue&>(o);
            std::string out = "class " + c.name;
            if (c.hasBase) out += "(" + vm.arena().deref(c.base).typeName() + ")";
            out += ":\n";
            // Walk the class + base chain, collecting the most-derived definition of each method.
            std::vector<std::pair<std::string, Handle>> chain;
            const ClassValue* cur = &c;
            fum::unordered_map<std::string, Handle> seen;
            while (true) {
                for (const auto& [k, v] : cur->methods)
                    if (!isPrivateName(k) && seen.find(k) == seen.end()) seen[k] = v;
                if (!cur->hasBase) break;
                cur = &static_cast<const ClassValue&>(vm.arena().deref(cur->base));
            }
            if (seen.empty()) return out + "  (no public methods)";
            std::string methods;
            for (const std::string& k : sortedKeys(seen)) methods += describe(k, seen[k], "  ");
            return out + methods.substr(0, methods.size() - 1);  // drop trailing newline
        } break;
        case ValueKind::Instance: {
            // A C++-authored NativeClass (Matrix, Random, File, …) also reports ValueKind::Instance
            // but is NOT an InstanceValue, so only treat genuine user-class instances as one;
            // anything else is described generically (never blindly downcast — that would read
            // garbage and crash).
            const auto* instp = dynamic_cast<const InstanceValue*>(&o);
            if (!instp) {
                // A native object: list the members it declares for introspection.
                std::vector<std::string> mem = o.inspectMembers();
                if (mem.empty()) return o.typeName() + " value";
                std::sort(mem.begin(), mem.end());
                std::string out = o.typeName() + " object:\n";
                for (const std::string& line : mem) out += "  " + line + "\n";
                return out.substr(0, out.size() - 1);
            }
            const auto& inst = *instp;
            std::string out = inst.className + " instance:\n";
            std::string attrs;
            for (const std::string& k : sortedKeys(inst.attrs))
                if (!isPrivateName(k)) attrs += describe(k, inst.attrs.at(k), "  attr ");
            // methods come from the class
            const auto& c = static_cast<const ClassValue&>(vm.arena().deref(inst.cls));
            fum::unordered_map<std::string, Handle> seen;
            const ClassValue* cur = &c;
            while (true) {
                for (const auto& [k, v] : cur->methods)
                    if (!isPrivateName(k) && seen.find(k) == seen.end()) seen[k] = v;
                if (!cur->hasBase) break;
                cur = &static_cast<const ClassValue&>(vm.arena().deref(cur->base));
            }
            std::string methods;
            for (const std::string& k : sortedKeys(seen)) methods += describe(k, seen[k], "  ");
            std::string body = attrs + methods;
            if (body.empty()) return out + "  (no public members)";
            return out + body.substr(0, body.size() - 1);
        } break;
        case ValueKind::Module: {
            const auto& mod = static_cast<const ModuleValue&>(o);
            std::string out = "module " + mod.name() + ":\n";
            std::string body;
            for (const std::string& k : sortedKeys(mod.members))
                // hide private members (single leading underscore, no trailing one — e.g. tensor's
                // grad-mode flag); public dunders like io.__stdout__ stay visible
                if (!(k.size() >= 1 && k.front() == '_' && k.back() != '_'))
                    body += describe(k, mod.members.at(k), "  ");
            if (body.empty()) return out + "  (empty)";
            return out + body.substr(0, body.size() - 1);
        } break;
        case ValueKind::Function: {
            return inspectSignature("function", static_cast<const KiFunction&>(o).def());
        } break;
        case ValueKind::NativeFunction: {
            const auto& nf = static_cast<const NativeFunction&>(o);
            return nf.hasSignature() ? inspectNativeSignature(vm, nf.name(), nf)
                                     : nf.name() + "(...)  [native]";
        } break;
        default: {
            // Built-in types (List/Dict/Set/Bytes/…) that declare members list them; scalars don't.
            std::vector<std::string> mem = o.inspectMembers();
            if (mem.empty()) return o.typeName() + " value";
            std::sort(mem.begin(), mem.end());
            std::string out = o.typeName() + " value:\n";
            for (const std::string& line : mem) out += "  " + line + "\n";
            return out.substr(0, out.size() - 1);
        } break;
    }
}

// Mini format-spec: [[fill]align][sign][#][0][width][,][.precision][type].
// Supports align <^>= , sign +/-/space, zero-pad, width, thousands ',', precision, and types
// b/o/x/X/d/f/e/g/s/% . Returns the formatted String. Throws on a malformed spec.
inline std::string applyFormatSpec(KiritoVM& vm, Handle value, const std::string& spec) {
    const Object& o = vm.arena().deref(value);
    std::size_t i = 0;
    char fill = ' ', align = 0, sign = '-';
    bool zero = false, comma = false, alt = false, hasSign = false;
    std::size_t width = 0;
    int precision = -1;
    char type = 0;
    // optional fill+align (fill only valid when an align char follows)
    auto isAlign = [](char c) { return c == '<' || c == '>' || c == '^' || c == '='; };
    if (i + 1 < spec.size() && isAlign(spec[i + 1])) { fill = spec[i]; align = spec[i + 1]; i += 2; }
    else if (i < spec.size() && isAlign(spec[i])) { align = spec[i]; ++i; }
    if (i < spec.size() && (spec[i] == '+' || spec[i] == '-' || spec[i] == ' ')) { sign = spec[i]; hasSign = true; ++i; }
    if (i < spec.size() && spec[i] == '#') { alt = true; ++i; }  // alternate form (base prefix for b/o/x)
    // The '0' flag zero-pads to the width. The '0' fill applies even when an explicit align is
    // given (format(7, ">06d") == "000007"); only an explicit FILL char overrides it. Default align
    // for a bare '0' is sign-aware '='.
    if (i < spec.size() && spec[i] == '0') { zero = true; if (fill == ' ') fill = '0'; if (!align) align = '='; ++i; }
    while (i < spec.size() && spec[i] >= '0' && spec[i] <= '9') {
        width = width * 10 + static_cast<std::size_t>(spec[i] - '0');
        if (width > kMaxRepeat) throw KiritoError("format width too large");
        ++i;
    }
    if (i < spec.size() && spec[i] == ',') { comma = true; ++i; }
    if (i < spec.size() && spec[i] == '.') {
        ++i;
        int64_t prec = 0;  // accumulate wide + bound each step so `int precision` can't overflow (UB)
        while (i < spec.size() && spec[i] >= '0' && spec[i] <= '9') {
            prec = prec * 10 + (spec[i] - '0');
            if (prec > static_cast<int64_t>(kMaxRepeat)) throw KiritoError("format precision too large");
            ++i;
        }
        precision = static_cast<int>(prec);
    }
    if (i < spec.size()) { type = spec[i]; ++i; }
    if (i != spec.size()) throw KiritoError("invalid format spec '" + spec + "'");

    auto groupThousands = [](std::string digits) {
        std::string out;
        int cnt = 0;
        for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
            if (cnt && cnt % 3 == 0) out += ',';
            out += *it; ++cnt;
        }
        std::reverse(out.begin(), out.end());
        return out;
    };

    std::string body, signStr;
    bool numeric = (o.kind() == ValueKind::Integer || o.kind() == ValueKind::Float || o.kind() == ValueKind::Bool);

    // An empty presentation type formats by value kind, matching `String()` (the no-type rule):
    // a Float uses general ('g') notation, a Bool stringifies to "True"/"False", an Integer stays
    // decimal, and anything else stringifies. (Without this, an empty-type Float/Bool wrongly fell into
    // the Integer branch below — `format(3.14)` thrown and `format(True)` gave "1".)
    if (type == 0) {
        if (o.kind() == ValueKind::Float) type = 'g';
        else if (o.kind() == ValueKind::Bool) type = 's';
    }

    if (type == 's' || (type == 0 && !numeric)) {
        // String presentation rejects numeric-only flags instead of silently ignoring them: a sign,
        // the '#' alternate form, sign-aware '=' alignment (also implied by a bare '0'), and ','
        // grouping are all meaningless for a String (matches Python's string format specifier rules).
        if (hasSign) throw KiritoError("format: sign not allowed in string format specifier");
        if (alt) throw KiritoError("format: alternate form '#' not allowed in string format specifier");
        if (align == '=') throw KiritoError("format: '=' alignment not allowed in string format specifier");
        if (comma) throw KiritoError("format: ',' not allowed in string format specifier");
        body = vm.stringify(value);
        if (precision >= 0 && static_cast<std::size_t>(precision) < utf8Length(body)) {
            auto starts = utf8Starts(body);
            body = body.substr(0, starts[precision]);
        }
        if (!zero && align == 0) align = '<';  // strings default to left-align
    } else if (type == 'b' || type == 'o' || type == 'x' || type == 'X' || type == 'd' || type == 0) {
        if (o.kind() != ValueKind::Integer && o.kind() != ValueKind::Bool)
            throw KiritoError("format type '" + std::string(1, type ? type : 'd') + "' needs an Integer");
        // Reject specs that an integer presentation cannot honor, rather than silently
        // dropping them: a precision is meaningless for an integer, and ',' grouping needs base 10.
        if (precision >= 0)
            throw KiritoError("format: precision not allowed for integer type '" + std::string(1, type ? type : 'd') + "'");
        int base0 = type == 'b' ? 2 : type == 'o' ? 8 : (type == 'x' || type == 'X') ? 16 : 10;
        if (comma && base0 != 10)
            throw KiritoError("format: ',' not allowed with format type '" + std::string(1, type) + "'");
        int64_t v = o.kind() == ValueKind::Bool ? (static_cast<const BoolVal&>(o).value() ? 1 : 0)
                                                : static_cast<const IntVal&>(o).value();
        bool neg = v < 0;
        uint64_t u = neg ? 0ull - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
        int base = type == 'b' ? 2 : type == 'o' ? 8 : (type == 'x' || type == 'X') ? 16 : 10;
        const char* alpha = type == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        std::string digits;
        if (u == 0) digits = "0";
        while (u) { digits += alpha[u % base]; u /= base; }
        std::reverse(digits.begin(), digits.end());
        if (comma && base == 10) digits = groupThousands(digits);
        signStr = neg ? "-" : (sign == '+' ? "+" : sign == ' ' ? " " : "");
        // `#` alternate form prepends the base prefix (after the sign, before any zero-padding:
        // format(255, "#08x") -> "0x0000ff"). Uppercase 'X' uses an uppercase "0X".
        if (alt && base != 10)
            signStr += base == 2 ? "0b" : base == 8 ? "0o" : (type == 'X' ? "0X" : "0x");
        body = digits;
    } else if (type == 'f' || type == 'e' || type == 'g' || type == '%') {
        if (o.kind() != ValueKind::Float && o.kind() != ValueKind::Integer && o.kind() != ValueKind::Bool)
            throw KiritoError("format type '" + std::string(1, type) + "' needs a number, not '" + o.typeName() + "'");
        double d = o.kind() == ValueKind::Float ? static_cast<const FloatVal&>(o).value()
                 : o.kind() == ValueKind::Integer ? static_cast<double>(static_cast<const IntVal&>(o).value())
                 : (static_cast<const BoolVal&>(o).value() ? 1.0 : 0.0);
        if (type == '%') d *= 100.0;
        bool neg = std::signbit(d);
        char fmtbuf[16];
        char conv = type == '%' ? 'f' : type;
        std::snprintf(fmtbuf, sizeof(fmtbuf), "%%.%d%c", precision < 0 ? 6 : precision, conv);
        // fmtbuf is built above from a fixed pattern + validated conv char; the dynamic format is
        // intentional. Size the output buffer to what snprintf actually needs (a high precision like
        // ".1000f" must NOT be silently truncated to a fixed 64-byte buffer).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
        int need = std::snprintf(nullptr, 0, fmtbuf, std::fabs(d));
        std::string out(need > 0 ? static_cast<std::size_t>(need) : 0, '\0');
        if (need > 0) std::snprintf(out.data(), static_cast<std::size_t>(need) + 1, fmtbuf, std::fabs(d));
#pragma GCC diagnostic pop
        body = std::move(out);
        if (type == '%') body += "%";
        if (comma) {
            std::size_t dot = body.find('.');
            std::string intpart = body.substr(0, dot);
            body = groupThousands(intpart) + (dot == std::string::npos ? "" : body.substr(dot));
        }
        signStr = neg ? "-" : (sign == '+' ? "+" : sign == ' ' ? " " : "");
    } else {
        throw KiritoError("unknown format type '" + std::string(1, type) + "'");
    }

    if (width > kMaxRepeat) throw KiritoError("format width too large");
    std::string s = signStr + body;
    if (utf8Length(s) >= width) return s;  // width counts CODE POINTS, not bytes (multibyte no longer under-pads)
    std::size_t pad = width - utf8Length(s);
    if (align == '<') return s + std::string(pad, fill);
    if (align == '>') return std::string(pad, fill) + s;
    if (align == '^') return std::string(pad / 2, fill) + s + std::string(pad - pad / 2, fill);
    if (align == '=') return signStr + std::string(pad, fill) + body;  // pad between sign and digits
    return std::string(pad, fill) + s;  // numbers default to right-align
}

// --- built-in globals ------------------------------------------------------------------------

// Fully iterate `src` and return its element handles, EACH registered as a GC root in `rs`. Every
// iterable-consuming builtin (map/filter/sorted/all/any/zip/enumerate, the Set/Dict constructors)
// runs a user callback or `_bool_`/`_lt_`/`_hash_` — or rehashes a container — mid-loop; a collection
// triggered by that allocation would sweep the elements not yet consumed (they live only in the
// returned vector), leaving dangling handles. Rooting the whole snapshot up front is the single guard
// against that, so every consumer routes through here instead of re-deriving the pattern. `err` is the
// "not iterable" diagnostic thrown when `src` has no iteration protocol.
inline std::vector<Handle> rootedIterate(KiritoVM& vm, Handle src, RootScope& rs, const char* err) {
    auto items = vm.arena().deref(src).iterate(vm);
    if (!items) throw KiritoError(err);
    for (Handle h : items.value()) rs.add(h);
    return std::move(items.value());
}

inline void KiritoVM::installBuiltins() {
    auto& g = static_cast<EnvValue&>(arena_.deref(global_));
    auto def = [&](const char* name, NativeFn fn) {
        g.define(arena(), name, alloc(std::make_unique<NativeFunction>(name, std::move(fn))));
    };
    // Like def, but with a declared signature so the builtin accepts keyword arguments and defaults
    // and describes itself under `inspect`.
    auto defSig = [&](const char* name, std::vector<NativeParam> sig, std::string ret, NativeFn fn) {
        g.define(arena(), name, alloc(std::make_unique<NativeFunction>(name, std::move(sig), std::move(ret), std::move(fn))));
    };

    defSig("len", {{"x"}}, "Integer", [](KiritoVM& vm, std::span<const Handle> args) -> Handle {
        if (args.size() != 1) throw KiritoError("len expected 1 argument");
        return vm.makeInt(vm.arena().deref(args[0]).length(vm).value());
    });

    defSig("import", {{"name", "String"}}, "Module", [](KiritoVM& vm, std::span<const Handle> args) -> Handle {
        if (args.size() != 1) throw KiritoError("import expected 1 argument");
        const Object& a = vm.arena().deref(args[0]);
        if (a.kind() != ValueKind::String) throw KiritoError("import expects a String module name");
        return vm.importModule(static_cast<const StrVal&>(a).value());
    });

    // Type constructors double as converters: Integer("42"), String(n), ...
    // Signatured so they accept a keyword arg (Integer(x = "42")) and describe themselves to inspect.
    defSig("Integer", {{"x"}}, "Integer", [](KiritoVM& vm, std::span<const Handle> args) -> Handle {
        if (args.size() != 1) throw KiritoError("Integer expected 1 argument");
        const Object& o = vm.arena().deref(args[0]);
        switch (o.kind()) {
            case ValueKind::Integer: { return args[0]; } break;
            case ValueKind::Bool: { return vm.makeInt(static_cast<const BoolVal&>(o).value() ? 1 : 0); } break;
            case ValueKind::Float: {
                double d = static_cast<const FloatVal&>(o).value();
                // Casting a non-finite or out-of-range double to int64 is UB; reject it cleanly.
                if (std::isnan(d)) throw KiritoError("cannot convert Float NaN to Integer");
                if (std::isinf(d)) throw KiritoError("cannot convert Float infinity to Integer");
                if (d >= 9223372036854775808.0 || d < -9223372036854775808.0)
                    throw KiritoError("Float is out of Integer range");
                return vm.makeInt(static_cast<int64_t>(d));
            } break;
            case ValueKind::String: {
                const std::string& s = static_cast<const StrVal&>(o).value();
                try {
                    // Find the value's start (skip leading whitespace + optional sign)
                    std::size_t i = 0;
                    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
                    bool neg = false;
                    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { neg = (s[i] == '-'); ++i; }
                    // Detect base from a 0x/0X/0o/0O/0b/0B prefix; default base 10.
                    int base = 10;
                    if (i + 1 < s.size() && s[i] == '0') {
                        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i + 1])));
                        if (c == 'x') { base = 16; i += 2; }
                        else if (c == 'o') { base = 8; i += 2; }
                        else if (c == 'b') { base = 2; i += 2; }
                    }
                    if (i == s.size() || (base != 10 && i == 0))
                        throw std::invalid_argument("empty value");
                    // std::stoll independently re-skips whitespace and re-parses a sign, which would
                    // let "--5", "+ 5", "0x-5", "0b -1" slip through after we already consumed our own
                    // sign/prefix. Require an actual base digit right here so such malformed input is
                    // rejected by the Integer() constructor.
                    auto isBaseDigit = [base](char ch) -> bool {
                        if (ch >= '0' && ch <= '9') return (ch - '0') < (base < 10 ? base : 10);
                        char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        return base > 10 && lc >= 'a' && lc < 'a' + (base - 10);
                    };
                    if (!isBaseDigit(s[i])) throw std::invalid_argument("no digit after sign/prefix");
                    // std::stoull(base 16) itself re-accepts a "0x" prefix, so a DOUBLED prefix like
                    // "0x0x5" would slip through and parse as 5. Reject an embedded prefix explicitly.
                    if (base == 16 && i + 1 < s.size() && s[i] == '0' &&
                        std::tolower(static_cast<unsigned char>(s[i + 1])) == 'x')
                        throw std::invalid_argument("embedded base prefix");
                    std::size_t pos = 0;
                    // Parse the magnitude as unsigned and bit-cast (two's-complement negate if signed),
                    // mirroring the lexer's intLiteral, so the full 64-bit range round-trips:
                    // Integer(String(INT64_MIN)), Integer(hex(-1)) == 0xFFFFFFFFFFFFFFFF == -1, etc.
                    // (std::stoll would reject any magnitude >= 2^63.)
                    uint64_t mag = std::stoull(s.substr(i), &pos, base);
                    // Reject trailing garbage (e.g. "42abc", "12.5") — surrounding whitespace allowed.
                    std::size_t end = i + pos;
                    while (end < s.size() && std::isspace(static_cast<unsigned char>(s[end]))) ++end;
                    if (end != s.size()) throw std::invalid_argument("trailing");
                    return vm.makeInt(static_cast<int64_t>(neg ? (~mag + 1ULL) : mag));
                } catch (...) {
                    throw KiritoError("cannot convert String to Integer: '" + s + "'");
                }
            } break;
            default: { throw KiritoError("cannot convert '" + o.typeName() + "' to Integer"); } break;
        }
    });

    defSig("Float", {{"x"}}, "Float", [](KiritoVM& vm, std::span<const Handle> args) -> Handle {
        if (args.size() != 1) throw KiritoError("Float expected 1 argument");
        const Object& o = vm.arena().deref(args[0]);
        switch (o.kind()) {
            case ValueKind::Float: { return args[0]; } break;
            case ValueKind::Integer: {
                return vm.makeFloat(static_cast<double>(static_cast<const IntVal&>(o).value()));
            } break;
            case ValueKind::Bool: { return vm.makeFloat(static_cast<const BoolVal&>(o).value() ? 1.0 : 0.0); } break;
            case ValueKind::String: {
                const std::string& s = static_cast<const StrVal&>(o).value();
                try {
                    // strtod accepts C99 hex-float syntax ("0x1p4"); the Float() constructor rejects it.
                    // Refuse the 0x/0X prefix (decimal "inf"/"nan" still parse).
                    std::size_t j = 0;
                    while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
                    if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
                    if (j + 1 < s.size() && s[j] == '0' && (s[j + 1] == 'x' || s[j + 1] == 'X'))
                        throw std::invalid_argument("hex float literal");
                    std::size_t pos = 0;
                    double v = parseDouble(s, &pos);
                    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
                    if (pos != s.size()) throw std::invalid_argument("trailing");
                    return vm.makeFloat(v);
                } catch (...) {
                    throw KiritoError("cannot convert String to Float: '" + s + "'");
                }
            } break;
            default: { throw KiritoError("cannot convert '" + o.typeName() + "' to Float"); } break;
        }
    });

    defSig("String", {{"x"}}, "String", [](KiritoVM& vm, std::span<const Handle> args) -> Handle {
        if (args.size() != 1) throw KiritoError("String expected 1 argument");
        return vm.makeString(vm.stringify(args[0]));
    });

    defSig("Bool", {{"x"}}, "Bool", [](KiritoVM& vm, std::span<const Handle> args) -> Handle {
        if (args.size() != 1) throw KiritoError("Bool expected 1 argument");
        return vm.makeBool(vm.arena().deref(args[0]).truthy());
    });

    // Bytes(x[, encoding]) — a raw byte sequence from a List of Integers (0..255), an Integer n
    // (n zero bytes), a String (encoded; default utf-8), or another Bytes (copied).
    defSig("Bytes", {{"x", "", none()}, {"encoding", "", none()}}, "Bytes", [](KiritoVM& vm, std::span<const Handle> args) -> Handle {
        if (args.empty() || vm.arena().deref(args[0]).kind() == ValueKind::None)
            return vm.alloc(std::make_unique<BytesVal>(std::string()));  // initialized empty value, not the deserializer shell
        std::string enc = "utf-8";
        if (args.size() > 1 && vm.arena().deref(args[1]).kind() != ValueKind::None)
            enc = Value(vm, args[1]).asStringRef("Bytes encoding");
        return makeBytes(vm, args[0], enc);
    });
    // Bytes.fromhex equivalent as a free builtin: fromhex("48 65") -> Bytes.
    defSig("fromhex", {{"s", "String"}}, "Bytes", [](KiritoVM& vm, std::span<const Handle> args) -> Handle {
        return vm.alloc(std::make_unique<BytesVal>(bytesutil::fromHex(Value(vm, args[0]).asStringRef("fromhex"))));
    });
    // Reconstruct a Bytes from its serialized (latin-1 String) state.
    registerDeserializer("Bytes", [](KiritoVM& vm, Handle) -> Handle {
        return vm.alloc(std::make_unique<BytesVal>());
    });

    // Collection constructors: List()/Set()/Dict() build an empty collection; List(iterable) and
    // Set(iterable) build from any iterable. (Literals [] {} {,} remain the idiomatic shorthand.)
    // The `iterable` parameter is keyword-callable (List(iterable = xs)); its None default means
    // "no source" -> an empty collection.
    defSig("List", {{"iterable", "", none()}}, "List", [](KiritoVM& vm, std::span<const Handle> args) -> Handle {
        RootScope rs(vm);
        auto list = std::make_unique<ListVal>();
        if (!args.empty() && vm.arena().deref(args[0]).kind() != ValueKind::None)
            list->elems = rootedIterate(vm, args[0], rs, "List() argument must be iterable");
        return vm.alloc(std::move(list));
    });
    defSig("Set", {{"iterable", "", none()}}, "Set", [](KiritoVM& vm, std::span<const Handle> args) -> Handle {
        RootScope rs(vm);
        Handle sh = rs.add(vm.alloc(std::make_unique<SetVal>()));
        auto& s = static_cast<SetVal&>(vm.arena().deref(sh));
        if (!args.empty() && vm.arena().deref(args[0]).kind() != ValueKind::None) {
            auto items = rootedIterate(vm, args[0], rs, "Set() argument must be iterable");
            for (Handle h : items) s.add(vm.arena(), h);
        }
        return sh;
    });
    defSig("Dict", {{"iterable", "", none()}}, "Dict", [](KiritoVM& vm, std::span<const Handle> args) -> Handle {
        RootScope rs(vm);
        Handle dh = rs.add(vm.alloc(std::make_unique<DictVal>()));
        auto& d = static_cast<DictVal&>(vm.arena().deref(dh));
        if (!args.empty() && vm.arena().deref(args[0]).kind() != ValueKind::None) {
            // Dict(pairs): each item is an iterable [key, value].
            auto items = rootedIterate(vm, args[0], rs, "Dict() argument must be iterable of pairs");
            for (Handle h : items) {
                auto pair = rootedIterate(vm, h, rs, "Dict() items must be [key, value] pairs");
                if (pair.size() != 2) throw KiritoError("Dict() items must be [key, value] pairs");
                d.set(vm.arena(), pair[0], pair[1]);   // key/value rooted by rootedIterate before set()
            }
        }
        return dh;
    });

    defSig("abs", {{"x", "Number"}}, "Number", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        const Object& o = vm.arena().deref(a[0]);
        if (o.kind() == ValueKind::Integer) {
            int64_t v = static_cast<const IntVal&>(o).value();
            // std::llabs(INT64_MIN) is UB; negate via unsigned for defined two's-complement
            // wraparound (abs(INT64_MIN) wraps to itself, consistent with Kirito's int64 semantics).
            return vm.makeInt(v < 0 ? wsub(0, v) : v);
        }
        if (o.kind() == ValueKind::Float) return vm.makeFloat(std::fabs(static_cast<const FloatVal&>(o).value()));
        throw KiritoError("abs expects a number");
    });
    defSig("round", {{"x", "Number"}, {"ndigits", "", none()}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        if (a.empty()) throw KiritoError("round expected at least 1 argument");
        const Object& xo = vm.arena().deref(a[0]);
        if (!isNumeric(xo)) throw KiritoError("round expects a number");
        double x = asDouble(xo);
        // ndigits given (and not the None default) -> round to that many decimals, yielding a Float;
        // otherwise round to the nearest Integer.
        if (a.size() >= 2 && vm.arena().deref(a[1]).kind() != ValueKind::None) {
            const Object& no = vm.arena().deref(a[1]);
            if (no.kind() != ValueKind::Integer) throw KiritoError("round ndigits must be an Integer");
            int64_t nd = static_cast<const IntVal&>(no).value();
            // Extreme ndigits: beyond a double's reach, pow(10, nd) would overflow to inf and yield
            // NaN. Rounding to more places than a double holds is a no-op; rounding past its magnitude
            // gives a signed zero (e.g. round(x, 10**18) / round(x, -10**18)).
            if (nd > 323) return vm.makeFloat(x);
            if (nd < -323) return vm.makeFloat(std::copysign(0.0, x));
            // Scale with long double so the intermediate x*f doesn't double-round (e.g. 2.675*100 in
            // plain double becomes 267.5 -> 268; in extended precision it stays 267.4999... -> 267,
            // i.e. round(2.675, 2) == 2.67). Keeps Kirito's documented half-away-from-zero rounding.
#if LDBL_MANT_DIG > DBL_MANT_DIG
            long double f = std::pow(10.0L, static_cast<long double>(nd));
            return vm.makeFloat(static_cast<double>(std::round(static_cast<long double>(x) * f) / f));
#else
            // No extended precision here (e.g. MSVC, where long double == double): the extended-precision
            // scaling trick above is a no-op, so `x * 10^nd` double-rounds and round(2.675, 2) would be
            // wrong. Fall back to the C library's correctly-rounded decimal conversion (platform-
            // independent) for nd >= 0; for negative nd, scaling in double is exact enough (10^|nd| is a
            // power of ten within double's integer range for the reachable |nd| <= 323).
            if (nd >= 0) {
                char buf[512];
                std::snprintf(buf, sizeof(buf), "%.*f", static_cast<int>(nd), x);
                return vm.makeFloat(std::strtod(buf, nullptr));
            }
            double f = std::pow(10.0, static_cast<double>(nd));
            double scaled = x * f;
            double r = (scaled >= 0.0) ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
            return vm.makeFloat(r / f);
#endif
        }
        if (std::isnan(x)) throw KiritoError("cannot round NaN to Integer");
        if (std::isinf(x)) throw KiritoError("cannot round infinity to Integer");
        if (x >= 9223372036854775808.0 || x < -9223372036854775808.0)
            throw KiritoError("rounded value out of Integer range");
        return vm.makeInt(static_cast<int64_t>(std::llround(x)));  // round(x) -> Integer
    });
    // range is variadic by position (range(stop) / range(start, stop) / range(start, stop, step))
    // but also names its three parameters: start, stop (alias end), and step. Positionals bind by the
    // classic count rule; keywords then fill or override any slot a positional didn't claim — a clash
    // (e.g. range(2, 5, start=1)) is an error, as is a missing stop.
    g.define(arena(), "range", alloc(std::make_unique<NativeFunction>("range",
        NativeFnKw([](KiritoVM& vm, std::span<const Handle> a, std::span<const NamedArg> named) -> Handle {
        auto iv = [&](Handle h) {
            const Object& o = vm.arena().deref(h);
            if (o.kind() != ValueKind::Integer) throw KiritoError("range expects Integers");
            return static_cast<const IntVal&>(o).value();
        };
        int64_t start = 0, stop = 0, step = 1;
        bool hasStart = false, hasStop = false, hasStep = false;
        // Keywords first, so the lone-positional-is-stop overload knows whether stop was named.
        for (const auto& na : named) {
            if (na.name == "start") {
                if (hasStart) throw KiritoError("range() got multiple values for 'start'");
                start = iv(na.value); hasStart = true;
            } else if (na.name == "stop" || na.name == "end") {
                if (hasStop) throw KiritoError("range() got multiple values for 'stop'");
                stop = iv(na.value); hasStop = true;
            } else if (na.name == "step") {
                if (hasStep) throw KiritoError("range() got multiple values for 'step'");
                step = iv(na.value); hasStep = true;
            } else throw KiritoError("range() got an unexpected keyword argument '" + na.name + "'");
        }
        if (a.size() > 3) throw KiritoError("range expects 1 to 3 positional arguments");
        if (a.size() == 1 && !hasStop) {
            stop = iv(a[0]); hasStop = true;          // range(stop) overload (when stop isn't a keyword)
        } else {                                       // otherwise positionals map 0->start, 1->stop, 2->step
            if (a.size() >= 1) { if (hasStart) throw KiritoError("range() got multiple values for 'start'");
                                 start = iv(a[0]); hasStart = true; }
            if (a.size() >= 2) { if (hasStop) throw KiritoError("range() got multiple values for 'stop'");
                                 stop = iv(a[1]); hasStop = true; }
            if (a.size() >= 3) { if (hasStep) throw KiritoError("range() got multiple values for 'step'");
                                 step = iv(a[2]); hasStep = true; }
        }
        if (!hasStop) throw KiritoError("range() missing required argument 'stop'");
        if (step == 0) throw KiritoError("range step cannot be zero");
        // range materializes a List (no lazy generators yet), so reject a count that would exhaust
        // memory up front rather than OOM mid-build.
        // Compute the span and count entirely in unsigned 64-bit: a signed `stop - start` overflows
        // when the endpoints straddle the int64 range (e.g. stop>0, start=INT64_MIN), and `-step`
        // overflows for step==INT64_MIN. The unsigned differences are exact since the true span fits
        // in uint64.
        // ceil(span / |step|) without the `span + |step| - 1` overflow (span can be up to 2^64-1 when
        // the endpoints straddle the int64 range, e.g. start=INT64_MIN, stop=INT64_MAX).
        uint64_t count = 0;
        if (step > 0 && stop > start) {
            uint64_t us = static_cast<uint64_t>(step);
            uint64_t span = static_cast<uint64_t>(stop) - static_cast<uint64_t>(start);
            count = span / us + (span % us != 0 ? 1 : 0);
        } else if (step < 0 && stop < start) {
            uint64_t negstep = 0u - static_cast<uint64_t>(step);  // |step|, valid even for INT64_MIN
            uint64_t span = static_cast<uint64_t>(start) - static_cast<uint64_t>(stop);
            count = span / negstep + (span % negstep != 0 ? 1 : 0);
        }
        if (count > kMaxRepeat) throw KiritoError("range too large");
        RootScope rs(vm);
        auto list = std::make_unique<ListVal>();
        list->elems.reserve(static_cast<std::size_t>(count));
        // Count-driven (not `i += step` until it passes `stop`): a near-INT64_MAX step would
        // signed-overflow that increment (UB) and not terminate. Every produced value is a valid
        // range element in [start, stop), so the per-element `v += step` can't overflow; we just skip
        // the final increment (which would step one past the last element and could overflow).
        int64_t v = start;
        for (uint64_t k = 0; k < count; ++k) {
            list->elems.push_back(rs.add(vm.makeInt(v)));
            if (k + 1 < count) v += step;
        }
        return vm.alloc(std::move(list));
    }))));
    defSig("sum", {{"iterable"}, {"start", "", makeInt(0)}}, "Number", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        auto items = vm.arena().deref(a[0]).iterate(vm);
        if (!items) throw KiritoError("sum() argument is not iterable");
        bool isFloat = false;
        double f = 0;
        int64_t n = 0;
        if (a.size() > 1) {  // optional start value (default 0): sum(it, start)
            const Object& st = vm.arena().deref(a[1]);
            if (st.kind() == ValueKind::Float) { isFloat = true; f = static_cast<const FloatVal&>(st).value(); }
            else if (st.kind() == ValueKind::Integer) { int64_t v = static_cast<const IntVal&>(st).value(); n = v; f = static_cast<double>(v); }
            else throw KiritoError("sum start must be a number");
        }
        for (Handle h : items.value()) {
            const Object& o = vm.arena().deref(h);
            if (o.kind() == ValueKind::Float) { isFloat = true; f += static_cast<const FloatVal&>(o).value(); }
            else if (o.kind() == ValueKind::Integer) { int64_t v = static_cast<const IntVal&>(o).value(); n = wadd(n, v); f += static_cast<double>(v); }
            else throw KiritoError("sum expects numbers");
        }
        return isFloat ? vm.makeFloat(f) : vm.makeInt(n);
    });
    // min/max are variadic (a single iterable, or several positional values) and accept the keyword
    // options `key` (a function producing the comparison key) and `default` (returned for an empty
    // single-iterable, else an empty sequence throws). inspect shows them as
    // variadic (...). who = "min"/"max".
    auto extremum = [](KiritoVM& vm, std::span<const Handle> a, std::span<const NamedArg> named,
                       bool wantMax, const char* who) -> Handle {
        RootScope rs(vm);
        Handle keyFn{};
        bool hasKey = false, hasDefault = false;
        Handle defaultVal{};
        for (const auto& na : named) {
            if (na.name == "key") { keyFn = na.value; hasKey = true; }
            else if (na.name == "default") { defaultVal = na.value; hasDefault = true; }
            else throw KiritoError(std::string(who) + "() got an unexpected keyword argument '" + na.name + "'");
        }
        std::vector<Handle> items;
        if (a.size() == 1) { auto it = vm.arena().deref(a[0]).iterate(vm);
            if (!it) throw KiritoError(std::string(who) + "() argument is not iterable");
            items = std::move(it.value()); }
        else items.assign(a.begin(), a.end());
        for (Handle h : items) rs.add(h);
        if (items.empty()) {
            if (hasDefault) return defaultVal;
            throw KiritoError(std::string(who) + "() arg is an empty sequence");
        }
        auto keyOf = [&](Handle h) -> Handle {
            if (!hasKey) return h;
            std::array<Handle, 1> args{h};
            return rs.add(vm.arena().deref(keyFn).call(vm, args));
        };
        Handle best = items[0], bestKey = keyOf(best);
        for (std::size_t i = 1; i < items.size(); ++i) {
            Handle k = keyOf(items[i]);
            bool better = wantMax ? kiLessThan(vm, bestKey, k) : kiLessThan(vm, k, bestKey);
            if (better) { best = items[i]; bestKey = k; }
        }
        return best;
    };
    g.define(arena(), "min", alloc(std::make_unique<NativeFunction>("min",
        NativeFnKw([extremum](KiritoVM& vm, std::span<const Handle> a, std::span<const NamedArg> n) {
            return extremum(vm, a, n, false, "min"); }))));
    g.define(arena(), "max", alloc(std::make_unique<NativeFunction>("max",
        NativeFnKw([extremum](KiritoVM& vm, std::span<const Handle> a, std::span<const NamedArg> n) {
            return extremum(vm, a, n, true, "max"); }))));
    defSig("sorted", {{"iterable"}, {"key", "", none()}, {"reverse", "Bool", makeBool(false)}}, "List",
           [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        // sorted(iterable[, key][, reverse]) -> a new STABLE-sorted List.
        RootScope rs(vm);
        Handle keyFn{};
        bool hasKey = false, reverse = false;
        if (a.size() > 1 && vm.arena().deref(a[1]).kind() != ValueKind::None) { keyFn = a[1]; hasKey = true; }
        if (a.size() > 2) reverse = vm.arena().deref(a[2]).truthy();
        auto items = rootedIterate(vm, a[0], rs, "sorted() argument is not iterable");
        std::vector<std::pair<Handle, Handle>> tagged;
        for (Handle h : items) {
            Handle k = h;
            if (hasKey) { std::array<Handle, 1> args{h}; k = rs.add(vm.arena().deref(keyFn).call(vm, args)); }
            tagged.emplace_back(k, h);
        }
        std::stable_sort(tagged.begin(), tagged.end(),
                         [&](const std::pair<Handle, Handle>& x, const std::pair<Handle, Handle>& y) {
                             return reverse ? kiLessThan(vm, y.first, x.first) : kiLessThan(vm, x.first, y.first);
                         });
        auto list = std::make_unique<ListVal>();
        for (auto& p : tagged) list->elems.push_back(p.second);
        return vm.alloc(std::move(list));
    });
    defSig("enumerate", {{"iterable"}, {"start", "", makeInt(0)}}, "List", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        RootScope rs(vm);
        auto out = std::make_unique<ListVal>();
        int64_t i = 0;
        if (a.size() > 1) {
            const Object& so = vm.arena().deref(a[1]);
            if (so.kind() != ValueKind::Integer)
                throw KiritoError("enumerate() start must be an Integer, got " + so.typeName());
            i = static_cast<const IntVal&>(so).value();
        }
        auto items = rootedIterate(vm, a[0], rs, "enumerate() argument is not iterable");
        for (Handle h : items) {
            auto pair = std::make_unique<ListVal>();
            // An index past the small-int intern range is a fresh allocation held ONLY by this
            // not-yet-arena-reachable pair, so vm.alloc(pair) below would collect it. (v1.15 A19-1.)
            pair->elems.push_back(rs.add(vm.makeInt(i)));
            i = wadd(i, 1);  // two's-complement wrap, no signed-overflow UB at INT64_MAX start
            pair->elems.push_back(h);
            out->elems.push_back(rs.add(vm.alloc(std::move(pair))));
        }
        return vm.alloc(std::move(out));
    });
    def("zip", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        RootScope rs(vm);
        std::vector<std::vector<Handle>> cols;
        std::size_t minLen = SIZE_MAX;
        for (Handle h : a) {
            auto col = rootedIterate(vm, h, rs, "zip() argument is not iterable");
            minLen = std::min(minLen, col.size());
            cols.push_back(std::move(col));
        }
        if (cols.empty()) minLen = 0;
        auto out = std::make_unique<ListVal>();
        for (std::size_t i = 0; i < minLen; ++i) {
            auto tup = std::make_unique<ListVal>();
            for (auto& c : cols) tup->elems.push_back(c[i]);
            out->elems.push_back(rs.add(vm.alloc(std::move(tup))));
        }
        return vm.alloc(std::move(out));
    });
    defSig("map", {{"function"}, {"iterable"}}, "List", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        RootScope rs(vm);
        Handle f = a[0];
        auto out = std::make_unique<ListVal>();
        auto items = rootedIterate(vm, a[1], rs, "map() argument is not iterable");
        for (Handle x : items) {
            std::array<Handle, 1> args{x};
            out->elems.push_back(rs.add(vm.arena().deref(f).call(vm, args)));
        }
        return vm.alloc(std::move(out));
    });
    defSig("filter", {{"function"}, {"iterable"}}, "List", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        RootScope rs(vm);
        Handle f = a[0];
        auto out = std::make_unique<ListVal>();
        auto items = rootedIterate(vm, a[1], rs, "filter() argument is not iterable");
        for (Handle x : items) {
            std::array<Handle, 1> args{x};
            if (vm.arena().deref(vm.arena().deref(f).call(vm, args)).truthy()) out->elems.push_back(x);
        }
        return vm.alloc(std::move(out));
    });
    defSig("type", {{"x"}}, "String", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        return vm.makeString(vm.arena().deref(a[0]).typeName());
    });
    defSig("id", {{"x"}}, "Integer", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        // Identity of an object: its arena slot — stable and unique for a live object (interned
        // scalars share one). Lets reference/cycle-aware code (e.g. a serializer) detect aliasing.
        return vm.makeInt(static_cast<int64_t>(a[0].slot));
    });
    defSig("inspect", {{"x"}}, "String", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        if (a.size() != 1) throw KiritoError("inspect expected 1 argument");
        return vm.makeString(inspectValue(vm, a[0]));
    });
    defSig("format", {{"value"}, {"spec", "String", makeString("")}}, "String",
           [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        // format(value[, spec]) -> String, applying a mini-format-spec. No spec == String().
        if (a.size() < 1 || a.size() > 2) throw KiritoError("format expected 1 or 2 arguments");
        std::string spec;
        if (a.size() == 2) {
            const Object& s = vm.arena().deref(a[1]);
            if (s.kind() != ValueKind::String) throw KiritoError("format spec must be a String");
            spec = static_cast<const StrVal&>(s).value();
        }
        // No spec == String(): route through stringify exactly like the f-string path
        // (bytecode_vm.hpp) so `format(2.0)` is `"2.0"`, not the `'g'`-lossy `"2"` that
        // applyFormatSpec's default would give (which doesn't round-trip a Float).
        if (spec.empty()) return vm.makeString(vm.stringify(a[0]));
        return vm.makeString(applyFormatSpec(vm, a[0], spec));
    });

    defSig("all", {{"iterable"}}, "Bool", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        if (a.size() != 1) throw KiritoError("all expected 1 argument");
        RootScope rs(vm);
        auto items = rootedIterate(vm, a[0], rs, "all expects an iterable");
        for (Handle h : items)
            if (!vm.arena().deref(h).truthy()) return vm.makeBool(false);   // a user _bool_ may allocate
        return vm.makeBool(true);
    });
    defSig("any", {{"iterable"}}, "Bool", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        if (a.size() != 1) throw KiritoError("any expected 1 argument");
        RootScope rs(vm);
        auto items = rootedIterate(vm, a[0], rs, "any expects an iterable");
        for (Handle h : items)
            if (vm.arena().deref(h).truthy()) return vm.makeBool(true);    // a user _bool_ may allocate
        return vm.makeBool(false);
    });
    defSig("reversed", {{"iterable"}}, "List", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        if (a.size() != 1) throw KiritoError("reversed expected 1 argument");
        RootScope rs(vm);
        auto items = rootedIterate(vm, a[0], rs, "reversed expects an iterable");
        auto out = std::make_unique<ListVal>();
        out->elems.assign(items.rbegin(), items.rend());
        return vm.alloc(std::move(out));
    });
    defSig("divmod", {{"a"}, {"b"}}, "List", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        if (a.size() != 2) throw KiritoError("divmod expected 2 arguments");
        RootScope rs(vm);
        Handle q = rs.add(numericBinary(vm, BinOp::FloorDiv, a[0], a[1]));
        Handle r = rs.add(numericBinary(vm, BinOp::Mod, a[0], a[1]));
        auto pair = std::make_unique<ListVal>();
        pair->elems.push_back(q);
        pair->elems.push_back(r);
        return vm.alloc(std::move(pair));
    });
    defSig("isinstance", {{"value"}, {"type"}}, "Bool", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        if (a.size() != 2) throw KiritoError("isinstance expected 2 arguments");
        // Second arg is a type: a class value, a built-in type constructor (Integer/Float/String/Bool/
        // Bytes/List/Set/Dict), or a String type-name. All resolve to a name matched through
        // typeMatches (kind names + inheritance-aware class chain).
        std::string typeName = resolveTypeName(vm, a[1]);
        if (typeName.empty())
            throw KiritoError("isinstance second argument must be a class, a built-in type, or a type-name String");
        return vm.makeBool(typeMatches(vm, a[0], typeName));
    });
    defSig("hasattr", {{"obj"}, {"name", "String"}}, "Bool", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        // hasattr(obj, name) -> does `obj.name` resolve to an attribute or method? True even when the
        // value is None (an attribute set to None still exists); False when the name is absent. Works
        // uniformly across every value: instances, classes, native/`.ki` modules, the built-in types
        // (Integer/String/List/... and their methods), and native objects.
        if (a.size() != 2) throw KiritoError("hasattr expected 2 arguments");
        const Object& n = vm.arena().deref(a[1]);
        if (n.kind() != ValueKind::String) throw KiritoError("hasattr: name (2nd argument) must be a String");
        std::string name = static_cast<const StrVal&>(n).value();   // copy: getAttr may allocate
        // Existence, not accessibility: probe the object's own attribute/method protocol slot, which
        // does NOT enforce privacy (that check lives in evalMemberGet, not here) — so a private member
        // reports as existing. getAttr runs no user code and has no side effects for any Kirito type,
        // so catching its throw is a clean "no such member" test.
        try {
            (void)vm.arena().deref(a[0]).getAttr(vm, a[0], name);
            return vm.makeBool(true);
        } catch (const std::exception&) {
            // Any failure to resolve the member is "does not exist" -> False. Catch std::exception,
            // not just KiritoError, so a native getAttr that throws a plain std::exception (crossing
            // the C++ boundary like a bare `catch` would absorb) reports absence instead of escaping.
            return vm.makeBool(false);
        }
    });
    defSig("ord", {{"char", "String"}}, "Integer", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        if (a.size() != 1) throw KiritoError("ord expected 1 argument");
        const Object& o = vm.arena().deref(a[0]);
        if (o.kind() != ValueKind::String) throw KiritoError("ord expects a String");
        const std::string& s = static_cast<const StrVal&>(o).value();
        if (utf8Length(s) != 1) throw KiritoError("ord expects a single character");
        return vm.makeInt(static_cast<int64_t>(utf8DecodeAt(s, 0)));
    });
    defSig("chr", {{"codepoint", "Integer"}}, "String", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        if (a.size() != 1) throw KiritoError("chr expected 1 argument");
        const Object& o = vm.arena().deref(a[0]);
        if (o.kind() != ValueKind::Integer) throw KiritoError("chr expects an Integer");
        int64_t cp = static_cast<const IntVal&>(o).value();
        if (cp < 0 || cp > 0x10FFFF) throw KiritoError("chr argument out of Unicode range");
        if (cp >= 0xD800 && cp <= 0xDFFF)
            throw KiritoError("chr argument is a UTF-16 surrogate (not a valid scalar code point)");
        std::string s;
        utf8Encode(static_cast<unsigned>(cp), s);
        return vm.makeString(s);
    });
    auto radix = [](KiritoVM& vm, std::span<const Handle> a, int base, const char* prefix,
                    const char* name) -> Handle {
        if (a.size() != 1) throw KiritoError(std::string(name) + " expected 1 argument");
        const Object& o = vm.arena().deref(a[0]);
        if (o.kind() != ValueKind::Integer) throw KiritoError(std::string(name) + " expects an Integer");
        int64_t v = static_cast<const IntVal&>(o).value();
        bool neg = v < 0;
        uint64_t u = neg ? 0ull - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
        std::string digits;
        const char* alpha = "0123456789abcdef";
        if (u == 0) digits = "0";
        while (u) { digits += alpha[u % base]; u /= base; }
        std::reverse(digits.begin(), digits.end());
        return vm.makeString((neg ? "-" : "") + std::string(prefix) + digits);
    };
    defSig("bin", {{"n", "Integer"}}, "String", [radix](KiritoVM& vm, std::span<const Handle> a) { return radix(vm, a, 2, "0b", "bin"); });
    defSig("oct", {{"n", "Integer"}}, "String", [radix](KiritoVM& vm, std::span<const Handle> a) { return radix(vm, a, 8, "0o", "oct"); });
    defSig("hex", {{"n", "Integer"}}, "String", [radix](KiritoVM& vm, std::span<const Handle> a) { return radix(vm, a, 16, "0x", "hex"); });
    // --- bitwise integer ops (Kirito has no &/|/^/~/<</>> operators; these builtins fill that role
    //     on Integers, operating on the full 64-bit two's-complement value) ---
    auto bint = [](KiritoVM& vm, Handle h, const char* fn, const char* who) -> int64_t {
        const Object& o = vm.arena().deref(h);
        if (o.kind() != ValueKind::Integer) throw KiritoError(std::string(fn) + ": " + who + " must be an Integer");
        return static_cast<const IntVal&>(o).value();
    };
    defSig("bitand", {{"a", "Integer"}, {"b", "Integer"}}, "Integer", [bint](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        return vm.makeInt(bint(vm, a[0], "bitand", "a") & bint(vm, a[1], "bitand", "b"));
    });
    defSig("bitor", {{"a", "Integer"}, {"b", "Integer"}}, "Integer", [bint](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        return vm.makeInt(bint(vm, a[0], "bitor", "a") | bint(vm, a[1], "bitor", "b"));
    });
    defSig("bitxor", {{"a", "Integer"}, {"b", "Integer"}}, "Integer", [bint](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        return vm.makeInt(bint(vm, a[0], "bitxor", "a") ^ bint(vm, a[1], "bitxor", "b"));
    });
    defSig("bitnot", {{"a", "Integer"}}, "Integer", [bint](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        return vm.makeInt(~bint(vm, a[0], "bitnot", "a"));  // ~a == -(a) - 1
    });
    defSig("shl", {{"a", "Integer"}, {"n", "Integer"}}, "Integer", [bint](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        int64_t x = bint(vm, a[0], "shl", "a"), n = bint(vm, a[1], "shl", "n");
        if (n < 0) throw KiritoError("shl: negative shift count");
        if (n >= 64) return vm.makeInt(0);  // all bits shifted out
        return vm.makeInt(static_cast<int64_t>(static_cast<uint64_t>(x) << n));  // wraps, no signed UB
    });
    defSig("shr", {{"a", "Integer"}, {"n", "Integer"}}, "Integer", [bint](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        int64_t x = bint(vm, a[0], "shr", "a"), n = bint(vm, a[1], "shr", "n");
        if (n < 0) throw KiritoError("shr: negative shift count");
        if (n >= 64) return vm.makeInt(x < 0 ? -1 : 0);  // arithmetic (sign-filling) shift
        return vm.makeInt(x >> n);  // C++20 guarantees arithmetic right shift for signed types
    });
    defSig("pow", {{"base"}, {"exp"}, {"mod", "", none()}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        // pow(base, exp) -> base**exp; pow(base, exp, mod) -> modular exponentiation. A None mod
        // (the default) means the plain two-argument form.
        if (a.size() < 3 || vm.arena().deref(a[2]).kind() == ValueKind::None)
            return numericBinary(vm, BinOp::Pow, a[0], a[1]);
        {
            // pow(base, exp, mod): modular exponentiation over non-negative Integers.
            auto geti = [&](Handle h, const char* w) {
                const Object& o = vm.arena().deref(h);
                if (o.kind() != ValueKind::Integer) throw KiritoError(std::string("pow ") + w + " must be Integer with 3 args");
                return static_cast<const IntVal&>(o).value();
            };
            int64_t base = geti(a[0], "base"), exp = geti(a[1], "exp"), mod = geti(a[2], "mod");
            if (exp < 0) throw KiritoError("pow exponent must be non-negative with a modulus");
            if (mod == 0) throw KiritoError("pow modulus must be non-zero");
            // A negative modulus would make `((base % mod) + mod) % mod` produce a silently-wrong
            // residue (C++ truncated `%` vs Kirito's floor `%`); reject it (docs scope this to
            // non-negative Integers) instead of returning a misleading number.
            if (mod < 0) throw KiritoError("pow modulus must be positive");
            // Reduce in __int128: `(base % mod) + mod` in int64 overflows for a modulus above ~2^62
            // (base%mod can be close to mod), silently corrupting the result. Widen first so the
            // normalization and every multiply stay exact.
            __extension__ __int128 m = mod;
            __extension__ __int128 result = 1 % m, b = ((static_cast<__int128>(base) % m) + m) % m;
            while (exp > 0) {
                if (exp & 1) result = (result * b) % mod;
                b = (b * b) % mod;
                exp >>= 1;
            }
            return vm.makeInt(static_cast<int64_t>(result));
        }
    });
}

// Register the bundled standard-library modules. Each line is a one-liner; a third party adds
// their own module exactly the same way: #include the module's header, then vm.install<T>().
inline void KiritoVM::installStandardLibrary() {
    install<IoModule>();
    install<PathModule>();
    install<MathModule>();
    install<RandomModule>();
    install<MatrixModule>();
    install<ComplexModule>();
    install<TensorModule>();
    install<JsonModule>();
    install<NetModule>();
    install<SerializeModule>();
    install<SysModule>();
    install<TimeModule>();
    install<DumpModule>();
    install<ZlibModule>();
    install<GzipModule>();
    install<HashModule>();
    install<CryptoModule>();
    install<IntModule>();
    install<RegexModule>();

    // Modules authored in Kirito and frozen into the binary (see stdlib_kimodules.hpp).
    registerSourceModule("itertools", kimod::itertools);
    registerSourceModule("functools", kimod::functools);
    registerSourceModule("collections", kimod::collections);
    registerSourceModule("statistics", kimod::statistics);
    registerSourceModule("string", kimod::string_mod);
    registerSourceModule("textwrap", kimod::textwrap);
    registerSourceModule("base64", kimod::base64);
    registerSourceModule("csv", kimod::csv);
    registerSourceModule("tabular", kimod::tabular);
    registerSourceModule("xml", kimod::xml);
    registerSourceModule("heapq", kimod::heapq);
    registerSourceModule("bisect", kimod::bisect);
    registerSourceModule("copy", kimod::copy_mod);
    registerSourceModule("enum", kimod::enum_mod);
    registerSourceModule("tee", kimod::tee);
    registerSourceModule("arg", kimod::arg);
    registerSourceModule("semver", kimod::semver);
}

inline void KiritoVM::retainChunk(std::unique_ptr<ast::Program> chunk) {
    chunks_.push_back(std::move(chunk));
}

inline KiritoVM::~KiritoVM() {
    // Remove exactly this VM from the thread's active stack, wherever it sits — so any destruction
    // order (LIFO or not) leaves activeVM() pointing at a still-live VM or null (A06-1).
    auto& s = _activeStack();
    s.erase(std::remove(s.begin(), s.end(), this), s.end());
}

inline Handle KiritoVM::evalIn(std::string_view source, Handle scope, std::string_view chunkName,
                              bool indexTopLevel) {
    try {
        ChunkFileScope chunkScope(*this, std::string(chunkName));  // functions defined here carry this file
        Lexer lexer(source);
        auto toks = lexer.tokenize();
        Parser parser(std::move(toks), lexer.source());  // source -> functions/classes capture their text
        auto prog = std::make_unique<ast::Program>(parser.parseProgram());
        const ast::Program& program = *prog;
        retainChunk(std::move(prog));  // keep the AST alive for the VM's lifetime (closures)
        if (!chunkName.empty()) programByFile_[std::string(chunkName)] = &program;  // for parallel.spawn span lookup
        Resolver(*this).resolve(program, scope, indexTopLevel);  // compile-time: every name must resolve, else NameError
        try {
            return runBytecodeBody(*this, scope, program.stmts, Handle{}, /*hasOwner=*/false,
                                   /*isFunction=*/false);
        } catch (const KiritoError&) {
            // KiritoError IS-A KiritoThrow — this pass-through keeps a C++-side error going
            // straight to the outer handler instead of the KiritoThrow arm below (which would
            // deref a Handle{} into stringify).
            throw;
        } catch (const KiritoThrow& t) {
            KiritoError err("uncaught exception: " + stringify(t.value), t.span);
            err.file = t.file;  // the defining chunk of the function that threw, if it escaped one
            err.traceback = t.traceback;  // carry the call chain so the CLI can print a traceback
            throw err;
        }
    } catch (KiritoError& e) {
        if (e.file.empty() && !chunkName.empty()) e.file = std::string(chunkName);
        throw;
    }
}

// A module/file scope under global, pre-bound with the per-file `arglist` and `argmain`. The
// command-line arguments belong to the program that was run, so `arglist` holds them only in a
// directly-run file (isMain); an imported module gets `argmain` False and an EMPTY `arglist`.
inline Handle KiritoVM::newModuleScope(bool isMain) {
    Handle scope = newScope(global_);
    RootScope rs(*this);
    rs.add(scope);
    Handle args;
    if (isMain) {
        if (!arglist_.slot) arglist_ = alloc(std::make_unique<ListVal>());  // default: no arguments
        args = arglist_;
    } else {
        args = alloc(std::make_unique<ListVal>());  // imported modules don't see the program's args
    }
    auto& env = static_cast<EnvValue&>(arena_.deref(scope));
    env.define(arena(), "arglist", args);
    env.define(arena(), "argmain", makeBool(isMain));
    return scope;
}

// Set the command-line arguments (called once by the embedder). They become the `arglist` bound in
// every module scope thereafter.
inline void KiritoVM::setArgs(const std::vector<std::string>& args) {
    RootScope rs(*this);
    auto list = std::make_unique<ListVal>();
    for (const auto& a : args) list->elems.push_back(rs.add(makeString(a)));
    arglist_ = alloc(std::move(list));
}

inline Handle KiritoVM::runSource(std::string_view source, std::string_view chunkName) {
    // A directly-run file is a genuine module scope: index its top-level bindings (LoadVar).
    return evalIn(source, newModuleScope(/*isMain=*/true), chunkName, /*indexTopLevel=*/true);
}

inline Handle KiritoVM::runRepl(std::string_view source) {
    if (!replScopeReady_) {
        replScope_ = newModuleScope(/*isMain=*/true);
        replScopeReady_ = true;
    }
    // The REPL's persistent scope is a genuine module scope treated exactly like a run file: it only
    // ever grows by append, so each line's top-level bindings keep stable slots and references to them
    // (including from closures defined on an earlier line) compile to direct LoadVar/AssignVar. The
    // resolver re-seeds its index map from the live scope each line, so cross-line names resolve.
    return evalIn(source, replScope_, {}, /*indexTopLevel=*/true);
}

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
