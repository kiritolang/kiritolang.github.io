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
