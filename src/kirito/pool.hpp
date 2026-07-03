#ifndef KIRITO_POOL_HPP
#define KIRITO_POOL_HPP

#include <cstddef>
#include <new>

#include "common.hpp"  // KIRITO_SANITIZER_BUILD

namespace kirito::pool {

// A thread-local, segregated free-list allocator for small objects (every Kirito `Object` — IntVal,
// FloatVal, EnvValue, ...). Profiling a tight arithmetic loop showed ~25% of run time in malloc/free:
// each `a + b` boxes a fresh IntVal, so the value churn hammers the general allocator. Recycling those
// fixed-size blocks instead removes that cost almost entirely.
//
// Why a *thread-local* free-list is correct (and needs no locking): Kirito's concurrency model is
// share-nothing multiprocessing — exactly one OS thread ever touches a given VM and its arena, and
// values cross VMs only as serialized blobs, never as live objects. So a block is always allocated and
// freed on the same thread; lists are never shared, and ThreadSanitizer stays clean.
//
// Under a sanitizer build the pool is bypassed entirely (straight ::operator new/delete) so
// AddressSanitizer/UBSan keep instrumenting every allocation — pooling would otherwise hide
// use-after-free / overflow within recycled blocks. The pool is a pure release-build speed win.

#if defined(KIRITO_SANITIZER_BUILD)

inline void* allocate(std::size_t n) { return ::operator new(n); }
inline void deallocate(void* p, std::size_t) noexcept { ::operator delete(p); }

#else

inline constexpr std::size_t kAlign = 16;                  // size-class granularity (>= alignof(max))
// Cover the hot value types AND the per-call EnvValue (208 B) and StrVal (88 B), which dominate
// call- and string-heavy code. 224 B is the largest pooled block; bigger objects (Tensors, etc.)
// bypass the pool. Each size class is just one thread-local pointer, so the upper bound is cheap.
inline constexpr std::size_t kMaxPooled = 224;             // bytes; larger objects bypass the pool
inline constexpr std::size_t kClasses = kMaxPooled / kAlign;  // 16,32,...,224-byte classes

struct FreeBlock { FreeBlock* next; };
inline thread_local FreeBlock* freeLists[kClasses] = {};

inline std::size_t classOf(std::size_t n) { return (n + kAlign - 1) / kAlign - 1; }  // 1..64 -> 0..3

inline void* allocate(std::size_t n) {
    if (n == 0) n = 1;
    if (n > kMaxPooled) return ::operator new(n);
    std::size_t c = classOf(n);
    if (FreeBlock* b = freeLists[c]) { freeLists[c] = b->next; return b; }
    return ::operator new((c + 1) * kAlign);  // fresh block, sized to its class so reuse is exact
}

// `n` is the complete-object size the compiler passes to the sized operator delete (verified to be
// correct for polymorphic deletion through a base with a virtual destructor).
inline void deallocate(void* p, std::size_t n) noexcept {
    if (!p) return;
    if (n == 0) n = 1;
    if (n > kMaxPooled) { ::operator delete(p); return; }
    FreeBlock* b = static_cast<FreeBlock*>(p);
    b->next = freeLists[classOf(n)];
    freeLists[classOf(n)] = b;
}

#endif

}  // namespace kirito::pool

#endif
