# A07 — arena + GC + object pool + handles (v1.14)

Scope: `src/kirito/arena.hpp`, `pool.hpp`, `handle.hpp`, the GC/rooting in `vm.hpp`
(`collectGarbage`, tempRoots/pinnedRoots/auxRoots/bytecodeConsts, adaptive threshold), and the
handle/rooting embedding API in `value.hpp` (RootScope, PinnedHandle, Pin).

Prior rounds: v1.13 `A08` and v1.12 `A08` (read). This file records only *new* findings and
explicit re-verifications requested by the plan. Read-only on `src/`. Probe binary:
`build-debug/ki` (pool BYPASSED — debug is a sanitizer-adjacent build; note where an **asan**/release
build is required to confirm a memory bug).

---

## Re-verifications requested by the plan

### RV-1: v1.13 GC-floor change (20k → 65536) — CORRECT, breaks no invariant
- location: `vm.hpp:364` `static constexpr std::size_t kGcThresholdFloor = 65536;` used at
  `vm.hpp:157` `gcThreshold_ = std::max(kGcThresholdFloor, arena_.liveCount() * 4);`
- verdict: **confirmed correct (non-bug).** The floor is a *lower bound on the allocation gap
  between collections*, and only binds when the live set is small (`liveCount*4 < 65536`, i.e.
  `liveCount < 16384`). Raising it makes the allocation-heavy/low-live case collect ~3× less often
  (pure frequency/perf change). It cannot cause an OOM under-collect: the collector still fires
  every 65536 allocations regardless of live set, and every root is still scanned each cycle;
  correctness (what is reclaimable) is unchanged, only *when*. It cannot over-collect. `gcAdaptive_`
  is only in effect when nobody pinned a threshold, and `setGcThreshold(1)` still forces
  per-allocation collection for tests (`gcAdaptive_=false`). No invariant broken.
- note: one latent interaction (see A07-6): the counter `allocsSinceGc_` is only incremented while
  `gcEnabled_`, so a large `GcPauseScope` burst leaves the counter low; harmless.

### RV-2: generation wraparound + Handle{} sentinel (v1.12 work) — CORRECT
- location: `arena.hpp:22` `kFirstGen=1`; `arena.hpp:62-68` retirement at `UINT32_MAX`;
  `handle.hpp:19-23` `Handle{slot=0,gen=0}` sentinel.
- verdict: **confirmed correct (non-bug).** First-ever alloc gets `{0,1}` (gen 1), so `Handle{}` =
  `{0,0}` can never alias a live object — `markIfUnmarked({0,0})`/`at({0,0})` fail the generation
  check against slot 0's real gen (≥1). A freed slot bumps generation (`++s.generation`) so a stale
  handle to a reused slot fails the `generation != h.generation` check. At `UINT32_MAX` the slot is
  *retired* (occupied=false, NOT pushed to `free_`) rather than wrapping to 0 — so gen 0 is never
  reissued and the sentinel invariant holds forever. A real value can never get generation 0.
- **coverage gap stands** (see A07-3): the retirement branch and the "stale-handle-vs-recycled-slot"
  ABA rejection are still not unit-tested (v1.13 A08-3 / A08-7, still open).

---

## New findings

### A07-1: Over-aligned `Object` subclass would be silently UNDER-aligned by the pool (latent UB, unguarded)
- severity: medium
- location: `object.hpp:82-83` (`static void* operator new(std::size_t n)` / sized `operator delete`), `pool.hpp:32` `kAlign=16`, `pool.hpp:59-65` `allocate`
- category: memory (latent UB) / robustness
- description: `Object` declares a member `operator new(std::size_t)` with **no** `std::align_val_t`
  overload. Once a class declares any member `operator new`, class-scope lookup **hides** the global
  allocation functions — including the global aligned `operator new(size_t, align_val_t)`. So if a
  future `Object` subclass is over-aligned (`alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__`, i.e. > 16
  on x86-64 — e.g. a member with `alignas(32)`, an `__m256`, a `std::complex<long double>` on some
  ABIs), the compiler is forced to allocate it through the non-aligned member `operator new(size_t)`,
  which routes to `pool::allocate`. The pool returns memory aligned only to `kAlign==16` (recycled
  blocks originate from `::operator new(size)` = default 16-align). The object is then constructed at
  an under-aligned address → **undefined behavior** (misaligned loads/stores; on the recycled-block
  path there is not even a fresh aligned allocation to save it).
- verification: confirmed **no current Object subclass triggers it** — the only over-alignment
  construct in `src/` is `alignas(Binding)` in `environment.hpp:116` where `alignof(Binding)==8 ≤ 16`,
  and `__STDCPP_DEFAULT_NEW_ALIGNMENT__ == 16 == kAlign` on the target. So this is **latent**, not a
  live bug. But nothing (no `static_assert`, no aligned-new overload) prevents a maintainer from
  adding an over-aligned value type and getting silent corruption — invisible to sanitizers, which
  **bypass the pool entirely** (so ASan/UBSan would allocate via the correct aligned global new and
  never see the misalignment). It is a release-build-only, sanitizer-invisible trap.
- failure-scenario: add a value type `struct SimdVal : Object { alignas(32) double lanes[4]; ... };`
  (≤224 B so it stays pooled) and allocate one — its 32-byte alignment requirement is silently
  dropped to 16; a SIMD aligned load faults or corrupts.
- proposed-test: (release build) a white-box unit test that defines a small over-aligned Object-like
  type routed through `pool::allocate` and `CHECK((reinterpret_cast<uintptr_t>(p) & 31) == 0)` — it
  will FAIL today, pinning the gap. (Needs release build; asan bypasses the pool.)
- proposed-fix: add the aligned overloads to `Object` that bypass the pool for over-aligned types —
  `static void* operator new(std::size_t n, std::align_val_t a) { return ::operator new(n, a); }`
  plus the matching sized `operator delete(void*, std::size_t, std::align_val_t)` — so an over-aligned
  subclass is allocated correctly (through global aligned new) instead of the 16-aligned pool. This is
  the standard-blessed way to keep a member `operator new` from hiding the aligned path. Optionally
  also `static_assert` at pool-boundary that pooled requests are ≤16-aligned.
- confidence: high (mechanism), latent (no current trigger)

### A07-2: GC root set is COMPLETE — re-verification (non-bug)
- severity: non-bug
- location: `vm.hpp:127-158` `collectGarbage` vs every Handle-holding VM member (`vm.hpp:306-346`)
- description: Cross-checked every Handle-typed member of `KiritoVM` against the roots enqueued in
  `collectGarbage`: `none_/true_/false_/undefined_/global_` (l131), `smallInts_` (l132), `replScope_`
  (l133, guarded by `replScopeReady_`), `moduleCache_` (l134), `pathCache_` (l135), `arglist_` (l136),
  `classRegistry_` (l137), `tempRoots_` (l138), `pinnedRoots_` (l139), `bytecodeConsts_` (l140),
  `auxRoots_` operand stacks (l141-142). Non-arena members hold no roots: `protoCache_` Protos store
  literals only as `bytecodeConsts_`-pinned handles; `chunks_`/`programByFile_` are AST (literals
  compiled to pinned consts, no live Handles); `nativeModules_`/`moduleFactories_` are C++ objects
  (their ModuleValue is reachable via `global_` or `moduleCache_`); **`deserializers_` std::functions
  all capture nothing** (verified: every `registerDeserializer` lambda is `[]`-captureless). No live
  value is reachable-but-unrooted. This confirms the mark phase is complete.

### A07-3: Small-int intern boundary correct, but `id(literal)==id(literal)` conflates interning with constant-dedup (test hazard)
- severity: coverage-gap
- location: `vm.hpp:69-72` `makeInt` (intern range `[-256,256]`), interacting with compiler constant
  deduplication (v1.9, `compiler.hpp`)
- category: coverage
- description: Probed the intern boundary two ways. With **literals** — `id(257)==id(257)` returns
  **True** (NOT because 257 is interned — it isn't — but because the compiler dedups the two `257`
  literals in one body to a single `consts` slot → same Handle). With **runtime-computed** values
  that defeat dedup — `id(200+57)==id(300-43)` (both 257) correctly returns **False**, and
  `id(200+56)==id(300-44)` (256) returns **True** — so the intern boundary itself is CORRECT
  (256/-256 interned, 257/-257 fresh). The hazard: any future regression test of the intern boundary
  written with literals (`id(257)==id(257)`) would spuriously pass via dedup and NOT actually test
  interning. v1.13 A08-4's proposed `id()` boundary test is only meaningful with runtime-computed
  operands. `-257` also happens to read False with literals because `-257` is `neg(257)` computed
  fresh each time, not a literal constant — another subtlety a naive test would misread.
- proposed-test: `.ki` assertions using arithmetic to defeat dedup:
  `id(200+56)==id(300-44)` (→True), `id(200+57)==id(300-43)` (→False), same for the negative end;
  document WHY literals can't be used. (No src fix — this is a correct-behavior + test-design note.)
- confidence: high

### A07-4: Arena `capacity()` (total slot count) stability still untested
- severity: coverage-gap
- location: `arena.hpp:80` `capacity()`, `arena.hpp:54-74` `sweep` free-list push
- category: coverage
- description: Still open from v1.13 A08-6. Every GC test asserts `liveCount()` stays bounded, but no
  test asserts `capacity()` (total `slots_.size()`) stabilizes. A bug that dropped `free_.push_back(i)`
  in `sweep` (so freed slots are never reused) would keep `liveCount` flat while `capacity` grows
  linearly with total allocations — a slow leak invisible to every current test. `capacity()` is
  never referenced in `tools/tests/`.
- proposed-test: C++ — `vm.setGcThreshold(1000)`, churn ~500k garbage boxes, assert
  `vm.arena().capacity()` after churn is within a small constant of its value after the first few
  collections (does NOT grow ~linearly with total allocations). Debug build suffices (pure arena
  logic, no pool).
- confidence: high

### A07-5: Small-object pool internals (classOf boundaries, block reuse, >224 bypass) still untested — release-only, sanitizer-invisible
- severity: coverage-gap
- location: `pool.hpp:57-76` (`classOf`, `allocate`, `deallocate`), boundaries `kAlign=16`, `kMaxPooled=224`
- category: coverage
- description: Still open from v1.13 A08-5 / v1.12. `test_gc_stress.cpp` churns boxes but only checks
  `liveCount`; nothing validates the pool's own invariants: (a) `classOf(n)==(n+15)/16-1` maps the
  boundary sizes 16/17/224/225 to the right class; (b) a >224-byte request bypasses the pool
  symmetrically in alloc AND dealloc (a mismatch would push a bypassed block onto a free-list, or
  hand a pooled block to a bypassed delete → corruption); (c) a freed block is reused for a same-class
  request (exact-size reuse). Because sanitizers **bypass the pool**, any pool corruption is
  **release-only and CI-invisible** — the single biggest untested memory surface. Also the alignment
  hazard A07-1 is only observable here.
- proposed-test: a **release-build** white-box unit over `kirito::pool::classOf` (boundary table
  16→0, 224→13, and that `allocate/deallocate` round-trip a canary-written block through a same-class
  reuse) plus a straddling interleave across 16/224/225 B with canary verification. Must run in a
  non-sanitizer build (add a CTest that is skipped under `KIRITO_SANITIZER_BUILD`).
- confidence: medium

### A07-6: `allocsSinceGc_` is not incremented while GC is paused (`GcPauseScope`) — benign, note only
- severity: low
- location: `vm.hpp:59` `if (gcEnabled_ && ++allocsSinceGc_ >= gcThreshold_)`
- category: perf (minor)
- description: The `++allocsSinceGc_` is short-circuited by `gcEnabled_`, so allocations made inside a
  `GcPauseScope` do NOT count toward the next auto-collection. After a large paused burst the counter
  under-reports actual allocation pressure, so the first post-resume collection fires later than the
  live footprint warrants (a transient over-retention, never an under-collect of *reachable* memory —
  correctness is unaffected). Distinct from `allocsSinceGc_ = 0` on collect (l152), which is correct.
- proposed-fix: none required (document); if desired, increment the counter unconditionally and gate
  only the `collectGarbage()` call on `gcEnabled_`.
- confidence: high

## Still-open coverage gaps carried from v1.13/v1.12 (not re-detailed)
- **Generation `UINT32_MAX` retirement branch** (`arena.hpp:62-68`) — dead in all tests; needs a
  friend/test-hook presetting a slot's generation to exercise "retire, never reissue". (v1.13 A08-3
  second half.) NOTE: the *ordinary* generation-bump + ABA-recycle path is now COVERED by
  `test_r7_embed_api.cpp:104-147` (v1.13 A08-3/A08-7 CLOSED) — only the max-gen retirement remains.
- **`EnvValue`/`SmallVec` inline→heap spill at kInline=4, copy ctor/assign, deep-chain lookup**
  (`environment.hpp`) — no direct unit test (v1.13 A08-8). Still open.

## Persisting minor items (already reported; confirmed NOT fixed in v1.14 src)
- `pool.hpp:57` stale comment `// 1..64 -> 0..3` (real mapping is 1..224 -> 0..13). v1.13 A08-14, still present.
- `vm.hpp:136` `if (arglist_.slot)` sentinel couples to "none_ occupies slot 0"; `arglist_ != Handle{}`
  or an explicit bool would be self-evidently correct like `replScopeReady_`. v1.13 A08-13, still present.
- `handle.hpp:33-35` `std::hash<Handle>` `<< 32` degenerates on a 32-bit `size_t`. v1.13 A08-12, still present.
- `runtime.hpp:3512` `_activeVM` non-LIFO multi-VM-per-thread dtor restore can dangle. v1.13 A08-1, still present.
