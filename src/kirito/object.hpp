#ifndef KIRITO_OBJECT_HPP
#define KIRITO_OBJECT_HPP

#include <cstdint>
#include <memory>
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
    // `Array` is a RESERVED kind with no producer today — there is no ArrayVal, so nothing ever
    // reports it. It survives only as a defensive `|| ValueKind::Array` alongside `List` in the
    // list-handling paths (a placeholder for a possible future List-model type). NOT a live type.
    Array, List, Set, Dict,
    Function, NativeFunction, Module, Class, Instance,
    Environment,
    // Lazy pull-based sequences (range/map/filter/zip/enumerate). Opaque and non-castable — they are
    // iterated, indexed, etc. only through the Object protocol slots, never by a kind-guarded downcast.
    Iterator,
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
    // Expose any Kirito Handles this iterator holds (a mapped function, source iterators, a user
    // iterator object) so the GC keeps them alive across an allocating next() — the IterCursor that
    // owns the LazyIterator calls this from children(). Default: none (a stream reads from its rooted
    // source and holds no independent handle).
    virtual void roots(std::vector<Handle>&) {}
};

// A dirty-card set over a container's ordered child array (List/Dict/Set). The write barrier marks the
// card containing a written entry; a minor rescans only dirty cards, so a container grown by appends is
// strictly O(N) at any nursery size (vs. the full re-children() rescan that was O(N^2/nursery)). Fixed
// card size (kCardSlots, well below kMinorNursery) is what guarantees the linear bound. The inline word
// covers the first 64 cards (8192 entries) with zero heap allocation; the spill scales to any size and
// is allocated only when a container both exceeds 8192 entries AND is written while old-and-remembered.
// `allDirty` is the O(1) fallback for an index-shifting mutation (insert/erase/reverse/sort/compaction).
struct CardTable {
    static constexpr std::size_t kCardSlots = 128;   // entries per card
    uint64_t inlineBits = 0;                          // cards [0,64)  -> entries [0, 8192)
    std::unique_ptr<std::vector<uint64_t>> spill;     // cards [64,..) -> allocated on demand
    bool allDirty = false;

    void mark(std::size_t entryIndex) {
        if (allDirty) return;
        std::size_t card = entryIndex / kCardSlots, word = card >> 6, bit = card & 63;
        if (word == 0) { inlineBits |= (uint64_t{1} << bit); return; }
        std::size_t si = word - 1;
        if (!spill) spill = std::make_unique<std::vector<uint64_t>>();
        if (spill->size() <= si) spill->resize(si + 1, 0);
        (*spill)[si] |= (uint64_t{1} << bit);
    }
    void markAll() { allDirty = true; }
    void clear() {
        inlineBits = 0; allDirty = false;
        if (spill) for (uint64_t& w : *spill) w = 0;
    }
    bool any() const {
        if (allDirty || inlineBits) return true;
        if (spill) for (uint64_t w : *spill) if (w) return true;
        return false;
    }
    // Call emit(lo, hi) for each dirty [lo, hi) entry range, clamped to `count` (the current child
    // count — so a container that SHRANK never emits a stale out-of-range slot).
    template <class F>
    void forEachDirtyRange(std::size_t count, F emit) const {
        if (allDirty) { if (count) emit(std::size_t{0}, count); return; }
        auto emitWord = [&](std::size_t wordIdx, uint64_t bits) {
            while (bits) {
                std::size_t card = wordIdx * 64 + static_cast<std::size_t>(__builtin_ctzll(bits));
                bits &= bits - 1;
                std::size_t lo = card * kCardSlots;
                if (lo >= count) continue;
                std::size_t hi = lo + kCardSlots; if (hi > count) hi = count;
                emit(lo, hi);
            }
        };
        emitWord(0, inlineBits);
        if (spill) for (std::size_t si = 0; si < spill->size(); ++si) emitWord(si + 1, (*spill)[si]);
    }
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
    // The SIZED aligned delete only exists when the compiler enables sized deallocation (GCC on by
    // default; Clang only under -fsized-deallocation). Guard it so `kirito.hpp` compiles under clang++
    // with default flags — the unsized aligned delete above already covers the over-aligned case
    // (dropping the size hint is always legal), so #if-ing this out is zero-risk.
#if defined(__cpp_sized_deallocation)
    static void operator delete(void* p, std::size_t n, std::align_val_t a) noexcept {
        ::operator delete(p, n, a);
    }
#endif

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

    // Card-marking hooks for large ordered containers (List/Dict/Set). The 4-arg write barrier marks
    // the card of a written entry; a minor rescans only dirty cards via childrenInDirtyCards. Defaults
    // below keep Env/Instance/Module/Class on the full rescan (they grow only from source, never a
    // runtime loop, so they can't drive the quadratic). childrenInDirtyCards defaults to children().
    virtual void gcMarkCard(std::size_t /*entryIndex*/) {}
    virtual void gcMarkAllCards() {}
    virtual void gcClearCards() {}
    virtual void childrenInDirtyCards(std::vector<Handle>& out) const { children(out); }

    // --- generational GC metadata (managed by ObjectArena + the collector; not part of the value) ---
    // Every Object is born YOUNG (age 0). Surviving a collection PROMOTES it in place (age reaches
    // kGcOldAge => old); non-moving, so its Handle never changes. The mark bit is the reachable flag
    // for one collection cycle. `gcRemembered_` records that this (old) object currently sits in the
    // collector's remembered set (it holds an old->young edge), so the write barrier enrolls it at
    // most once per cycle. Pure bookkeeping — never affects a value's identity, equality, hash, or
    // serialization; the same value is young or old at different times with no observable difference.
    static constexpr uint8_t kGcOldAge = 1;   // promote-on-first-survival: the nursery is age 0 only
    bool gcYoung() const { return gcAge_ == 0; }
    uint8_t gcAge() const { return gcAge_; }
    void gcSetAge(uint8_t a) { gcAge_ = a; }
    void gcPromote() { if (gcAge_ < 0xFF) ++gcAge_; }
    bool gcMarked() const { return gcMarked_; }
    void gcSetMarked(bool m) { gcMarked_ = m; }
    bool gcRemembered() const { return gcRemembered_; }
    void gcSetRemembered(bool r) { gcRemembered_ = r; }

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

    // GC: does this object hold young handles in NON-barriered storage (a plain C++ buffer, not an arena
    // container), such that a minor collection must re-trace its children even when the object is OLD and
    // reachable only from an operand-stack root? The generational fast path assumes every old->young edge
    // is barriered into the remembered set; a lazy IterCursor's buffered source elements are the one
    // exception (they live in a std::vector inside the iterator, never touched by a write barrier). Only
    // that type overrides this to true; every arena container barriers its writes and returns false.
    virtual bool gcNeedsRootRescan() const { return false; }

private:
    // 3 bytes of generational bookkeeping; the vtable pointer already dominates every value's size,
    // so this stays within the small-object pool's existing size classes.
    uint8_t gcAge_ = 0;         // 0 = young (nursery); >= kGcOldAge = old (promoted)
    bool gcMarked_ = false;     // reachable in the current collection cycle
    bool gcRemembered_ = false; // enrolled in the collector's remembered set this cycle
};

// ---------------------------------------------------------------------------------------------------
// Generational write barrier. Storing a (possibly young) handle into a (possibly OLD) container must
// tell the collector about the resulting old->young edge, or a minor GC would free a still-reachable
// young object (use-after-free). The barrier lives INSIDE the core value mutators (EnvValue bindings,
// List/Dict/Set element writes, Instance/Module attribute writes, Class method installs), so it fires
// transparently for BOTH the interpreter and the high-level C++ API (value.hpp wrappers call the same
// mutators). It early-outs on the common case (a young container — fresh call scopes, freshly-built
// containers) with a single byte test and no arena access.
//
// Two overloads: one when the arena is already in hand (the collection/Dict/Set/Instance/Module
// mutators), one for EnvValue (which holds no arena — it reaches the arena via KiritoVM::activeVM()
// only on the rare old-container path). Declared here (Object is complete); DEFINED in runtime.hpp
// after KiritoVM is complete. Both are no-ops until the generational machinery is active.
class ObjectArena;
inline void gcWriteBarrier(ObjectArena& arena, Object* container, Handle value);
inline void gcWriteBarrier(Object* container, Handle value);
// Card-aware form for List/Dict/Set: besides remembering the old->young edge, it marks the written
// entry's card so a minor rescans only that region. Unlike the 3-arg form it must NOT early-out on
// gcRemembered() before marking — a container remembered at entry 5 and later written at entry 9000
// must dirty 9000's card too. Defined in runtime.hpp after KiritoVM is complete.
inline void gcWriteBarrier(ObjectArena& arena, Object* container, Handle value, std::size_t entryIndex);

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
