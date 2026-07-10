#ifndef KIRITO_OBJECT_HPP
#define KIRITO_OBJECT_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fum/unordered_set.hpp"
#include "common.hpp"
#include "handle.hpp"
#include "pool.hpp"

namespace kirito {

class ObjectArena;
class KiritoVM;
class Object;

enum class ValueKind {
    None, Bool, Integer, Float, String,
    Array, List, Set, Dict,
    Function, NativeFunction, Module, Class, Instance,
    Environment,
};

// Threaded through str() so containers can detect reference cycles (a value already being
// stringified) and emit an ellipsis instead of recursing forever. Keyed by object identity.
struct StringifyCtx {
    const ObjectArena& arena;
    fum::unordered_set<const Object*> active;
    KiritoVM* vm = nullptr;  // set when a user-defined _str_ may need to be invoked
    int depth = 0;           // nesting depth, bounded to keep a deep (acyclic) structure from
                             // overflowing the C++ stack — distinct from the `active` cycle guard.
};

// Structural equality recurses through nested containers; without a bound, two distinct cyclic
// structures would overflow the stack. This RAII guard caps the depth and throws a catchable
// error instead. (A transient thread-local counter, always balanced — not VM state.)
namespace detail {
inline thread_local int g_equalsDepth = 0;
}
struct EqualsGuard {
    static constexpr int kMaxDepth = 1000;   // bounds comparison recursion (equality AND ordering:
                                             // kiEquals and kiLessThan both use it, so the message
                                             // says "comparison", not "equality" — see A04-2)
    EqualsGuard() {
        if (++detail::g_equalsDepth > kMaxDepth) {
            --detail::g_equalsDepth;
            throw KiritoError("maximum comparison recursion depth exceeded (cyclic structure?)");
        }
    }
    ~EqualsGuard() { --detail::g_equalsDepth; }
    EqualsGuard(const EqualsGuard&) = delete;
    EqualsGuard& operator=(const EqualsGuard&) = delete;
};

// Optional lazy (pull-based) iteration. Most iterables materialize their elements up front via
// iterate(); a STREAM (a file/stdin/BytesIO) instead yields one element per next() so memory stays
// bounded and stdin is processed as it arrives (not buffered to EOF). A type opts in by overriding
// Object::lazyIterate to hand back one of these; the for-loop pulls next() until it returns nullopt.
struct LazyIterator {
    virtual ~LazyIterator() = default;
    virtual std::optional<Handle> next(KiritoVM& vm) = 0;  // nullopt == exhausted
};

// The one uniform protocol every value implements — built-in scalars, collections,
// C++-authored types, and (later) Kirito `class` instances. The evaluator only ever talks
// to this interface, so it cannot tell built-ins from user-defined objects apart.
//
// Operation slots (binary/unary/call/...) default to a clear "unsupported" error; concrete
// types override only what they support. Slots receive the KiritoVM so they can allocate
// results and dereference operands, and the caller's own Handle so they can reference self.
class Object {
public:
    virtual ~Object() = default;

    // Every value is heap-boxed and churns hard (a fresh IntVal per arithmetic op), so route all
    // Object allocation through the thread-local small-object pool (pool.hpp) instead of the general
    // allocator — profiling put ~25% of a tight loop's time in malloc/free. The sized delete receives
    // the complete-object size under polymorphic deletion via this virtual destructor. Bypassed under
    // sanitizers (pool.hpp) so ASan/UBSan still see every allocation.
    static void* operator new(std::size_t n) { return pool::allocate(n); }
    static void operator delete(void* p, std::size_t n) noexcept { pool::deallocate(p, n); }
    // Declaring a member operator new HIDES the global aligned allocation functions, so an
    // over-aligned Object subclass (alignof > kAlign, e.g. an `alignas(32)` SIMD member) would
    // otherwise be routed through the 16-aligned pool and constructed at an under-aligned address
    // (UB — sanitizer-invisible, since the pool is bypassed there). Re-expose the aligned path: an
    // over-aligned type bypasses the pool and uses global aligned new/delete, which honours its
    // alignment. No current type is over-aligned, so this is a defensive guard against a future one.
    static void* operator new(std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
    static void operator delete(void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
    static void operator delete(void* p, std::size_t n, std::align_val_t a) noexcept {
        ::operator delete(p, n, a);
    }

    virtual ValueKind kind() const = 0;
    virtual std::string typeName() const = 0;

    virtual bool truthy() const = 0;
    virtual std::string str(StringifyCtx&) const = 0;
    virtual bool equals(const ObjectArena&, const Object& other) const = 0;

    virtual bool hashable() const { return false; }
    virtual std::size_t hash() const { throw KiritoError("unhashable type '" + typeName() + "'"); }

    // True only for the native Bytes value. Bytes is a ValueKind::Instance (no dedicated kind), so the
    // embedding API must not discriminate it by typeName()=="Bytes" — a user `class Bytes` would then
    // be mis-cast to BytesVal. This virtual is the safe discriminator (A09-2).
    virtual bool isBytesValue() const { return false; }

    // Enumerate contained handles for the mark-sweep GC and for serialization.
    virtual void children(std::vector<Handle>&) const {}

    // Public members (methods + attributes) for `inspect`, as formatted one-line descriptions, e.g.
    // "randint(a, b) -> Integer" or "year: Integer". Because a native type's methods are produced
    // on demand in getAttr (there's no list to walk), a NativeClass declares them here so inspect can
    // show them; default is none (inspect then falls back to "<type> value").
    virtual std::vector<std::string> inspectMembers() const { return {}; }

    virtual Handle binary(KiritoVM&, BinOp, Handle self, Handle rhs);
    virtual Handle unary(KiritoVM&, UnOp, Handle self);
    virtual Handle call(KiritoVM&, std::span<const Handle> args);
    // getAttr receives the receiver's own handle so bound methods can capture (and GC-root) it.
    virtual Handle getAttr(KiritoVM&, Handle self, std::string_view name);
    virtual void setAttr(KiritoVM&, std::string_view name, Handle value);
    // Indexing supports any number of keys, so obj[x, y, z] works (e.g. matrices).
    virtual Handle getItem(KiritoVM&, std::span<const Handle> keys);
    virtual void setItem(KiritoVM&, std::span<const Handle> keys, Handle value);
    // start/stop/step are Integer handles or None for "omitted".
    virtual Handle slice(KiritoVM&, Handle start, Handle stop, Handle step);
    virtual std::optional<std::vector<Handle>> iterate(KiritoVM&);
    // Opt-in lazy iteration (streams). `self` is the receiver's handle so the iterator can re-deref it
    // each step. Default: none — the for-loop falls back to eager iterate().
    virtual std::unique_ptr<LazyIterator> lazyIterate(KiritoVM&, Handle /*self*/) { return nullptr; }
    virtual std::optional<int64_t> length(KiritoVM&);
    virtual bool contains(KiritoVM&, Handle value);  // the `in` operator
};

inline Handle Object::binary(KiritoVM&, BinOp, Handle, Handle) {
    throw KiritoError("type '" + typeName() + "' does not support this binary operator");
}
inline Handle Object::unary(KiritoVM&, UnOp, Handle) {
    throw KiritoError("type '" + typeName() + "' does not support this unary operator");
}
inline Handle Object::call(KiritoVM&, std::span<const Handle>) {
    throw KiritoError("type '" + typeName() + "' is not callable");
}
inline Handle Object::getAttr(KiritoVM&, Handle, std::string_view name) {
    throw KiritoError("type '" + typeName() + "' has no attribute '" + std::string(name) + "'");
}
inline void Object::setAttr(KiritoVM&, std::string_view name, Handle) {
    throw KiritoError("type '" + typeName() + "' has no attribute '" + std::string(name) + "'");
}
inline Handle Object::getItem(KiritoVM&, std::span<const Handle>) {
    throw KiritoError("type '" + typeName() + "' is not indexable");
}
inline void Object::setItem(KiritoVM&, std::span<const Handle>, Handle) {
    throw KiritoError("type '" + typeName() + "' does not support item assignment");
}
// Containers that take exactly one index validate arity and unwrap the single key here.
inline Handle singleKey(const Object& self, std::span<const Handle> keys) {
    if (keys.size() != 1)
        throw KiritoError("type '" + self.typeName() + "' takes exactly one index");
    return keys[0];
}
inline Handle Object::slice(KiritoVM&, Handle, Handle, Handle) {
    throw KiritoError("type '" + typeName() + "' does not support slicing");
}
inline std::optional<std::vector<Handle>> Object::iterate(KiritoVM&) {
    throw KiritoError("type '" + typeName() + "' is not iterable");
}
inline std::optional<int64_t> Object::length(KiritoVM&) {
    throw KiritoError("type '" + typeName() + "' has no length");
}
inline bool Object::contains(KiritoVM&, Handle) {
    throw KiritoError("type '" + typeName() + "' does not support 'in'");
}

}  // namespace kirito

#endif
