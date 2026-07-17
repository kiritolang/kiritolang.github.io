# A04 — GC subsystem audit (v1.16.1)

Scope: arena.hpp, pool.hpp, object.hpp, handle.hpp; GC parts of vm.hpp (alloc/minorCollect/
collectGarbage/forEachRoot/thresholds/pinHandle/tempRoots/auxRoots), the write barriers in
runtime.hpp (3-arg + 4-arg card form), GC-touching bits of bytecode_vm.hpp (IterCursor,
gcNeedsRootRescan, GetIter/ForIter rooting).

Overall: this subsystem is mature and has clearly survived 5 audit rounds + asan/tsan/ubsan gates.
Card marking, the generational invariant (old->young edges barriered-or-promoted), the
non-moving-arena reentrancy safety, and the lazy-iterator rescan are all sound on close reading.
I could not find a confirmed correctness bug via static analysis; findings below are one real
coverage gap in brand-new code and a couple of low-severity consistency/robustness notes.

## Findings

### F04-1  [Med]  CardTable spill path (>8192-entry container) has zero test coverage
- object.hpp:87-129 (CardTable::mark / forEachDirtyRange / clear, the `spill`
  std::unique_ptr<std::vector<uint64_t>> branch) — the inline word covers cards [0,64) = entries
  [0, 8192). A container with >8192 live entries that is OLD, remembered, and written past index
  8192 is the ONLY thing that allocates and exercises `spill`. No test in the suite builds such a
  container: the card tests (test_gc_generational.cpp §CARD MARKING, lines 341-397) top out at 400
  entries; the stress tests churn short-lived boxes or mutate small (≤500-entry) lists. So the spill
  index arithmetic (`word-1`, `spill->resize(si+1)`, the `emitWord(si+1, ...)` re-offset), the
  spill loop in `any()`/`clear()`, and the clamp when a >8192 container SHRINKS are all unverified by
  any test.
- Trigger/repro: an old, remembered List/Dict/Set grown past 8192 entries then written near the tail
  under `setGcThreshold(1)`. I hand-traced entryIndex=12000 -> card 93 -> word 1, bit 29 ->
  spill[0] bit 29 -> forEachDirtyRange emits [11904, 12032): arithmetic is CORRECT on inspection, so
  this is a coverage gap, not a known bug — but it's brand-new code guarding a UAF and nothing tests it.
- Fix idea: no code change; add a test.
- Test to add: under `setGcThreshold(1)`, build a module-level List of >9000 fresh non-interned
  values (e.g. `"v"+String(i)`), let it promote to old, then `xs[9000] = "sentinel"` and
  `xs.append("tail")`, churn allocations, and assert `xs[9000]`/`xs[last]` survive. Repeat for Dict
  and Set (>9000 keys, overwrite a value past card 64, verify survival). This is the direct
  regression for the spill branch.
- Verified-real: coverage gap confirmed (grepped every tests/unit/test_gc*.cpp and stress loops —
  largest single card-tracked container is 400 entries; no `spill` allocation is ever forced). Spill
  arithmetic hand-verified correct, so no functional bug asserted.

### F04-2  [Low]  ListVal.clear() (and value.hpp List::clear) don't reset the CardTable, unlike Dict/Set
- runtime.hpp:732 (`self_list(vm, self).elems.clear();`) and value.hpp:622
  (`void clear() { mut().elems.clear(); }`) clear `elems` but leave `cards` untouched. DictVal::clear
  (collections.hpp:360-367) and SetVal::clear (484-491) both call `cards.clear()`. The List path is
  the odd one out.
- Why it's benign but worth noting: `forEachDirtyRange` clamps every range to the current child count
  and `markIfYoungUnmarked` no-ops on already-old children, so a stale `allDirty`/dirty-card after a
  clear+refill can only cause the next minor to rescan more (already-old) entries — never a lost young
  edge or an out-of-bounds read. It defeats the O(N) card optimization on a cleared-then-refilled old
  List until the next `resetRemembered()`, i.e. a small perf-only wart plus an API inconsistency.
- Fix idea: add `cards.clear();` to both List clear sites, matching Dict/Set (one line each). Or make
  `ListVal` expose a `clearElems()` mutator that both call, so the invariant is single-sourced.
- Test to add: after clearing and refilling a large old List under GC pressure, assert the minor still
  reclaims correctly (already covered by survival, so this is mainly a consistency cleanup).
- Verified-real: yes — read both clear paths; Dict/Set clear cards, List does not. Impact is perf/
  consistency only (clamp + old-child no-op make it memory-safe).

## Coverage notes (untested methods/paths — candidates for new tests, not bugs)

- **CardTable spill (>8192 entries)** — see F04-1. The single biggest untested surface in the
  round's new code.
- **CardTable::any()** — appears unused by the GC hot path (grep finds no caller in vm.hpp/
  collections.hpp/runtime.hpp). Either dead code or only-used-in-a-would-be-test. Worth confirming
  it's needed; if kept, it's untested (esp. its spill loop).
- **pool.hpp over-alignment path** (object.hpp:156-166 aligned operator new/delete) — deliberately
  defensive; no over-aligned Object subtype exists, so the aligned allocation functions are never
  selected at runtime and cannot be exercised without adding an `alignas(32)` test type. Documented as
  defensive, so acceptable, but literally 0% covered.
- **Slot generation wraparound retirement** (arena.hpp:181-186) — the `generation == UINT32_MAX`
  branch that permanently retires a slot to prevent ABA is unreachable in practice (needs 2^32 reuses
  of one slot). Untestable at runtime; could be unit-tested by exposing/forcing a slot's generation in
  a white-box test if desired. Low value.
- **Multi-VM write-barrier routing (A05-1)** — the arena-less barrier overload was removed, so the
  "VM_B answers about a VM_A handle" misroute is now structurally impossible (every mutator takes the
  owning arena from its caller). No runtime test can trigger it and none is needed; noting only that
  the guarantee is compile-time (test_arena.cpp's two-VM case only checks independence, not barrier
  routing under GC pressure with two live VMs on one thread).
- **gcNeedsRootRescan promotion path** — IS exercised: with `setGcThreshold(1)`, even the 4-8 element
  lazy-iterator tests (test_gc_generational.cpp §LAZY ITERATORS) promote the IterCursor to old after
  the first minor, so the old-IterCursor rescan in minorCollect runs. Adequate.
- **Dict/Set compaction markAll under card marking** — covered (test §(e), deletes force
  tombstones + rehash-compaction). Good.
- **List insert-not-at-end / reverse / sort / pop-mid / remove markAll** — covered (test §(c)). Good.
- **collectGarbage old-spanning-cycle reclamation** — covered (test_gc_stress §GENERATIONAL cycle).

## Areas examined and judged sound (no finding)

- Generational invariant: every old->young edge is either (a) created by a barriered mutator that
  calls `remember()` (List/Dict/Set 4-arg, Env/Instance/Module/Class 3-arg), or (b) an
  immutable-after-construction child (KiFunction closure/ownerClass, NativeFunction captures/defaults,
  InstanceValue.cls, ClassValue base/closure, EnvValue.parent) which is traced+promoted with its owner
  while young and never re-mutated. ClassValue has no runtime setAttr, so class attrs can't gain a
  young child post-promotion. Consistent.
- `resetRemembered()`/`sweep()` clear the remembered flag and the `remembered_` vector in lockstep;
  the promote-on-first-survival invariant (kGcOldAge==1, static_assert'd) makes the wholesale clear
  sound. No dangling `Object*` in `remembered_` is ever dereferenced (major clears it without deref;
  minor only holds live old objects).
- Non-moving arena makes the ProbeScope reentrancy model correct: a user `_hash_`/`_eq_`/`_str_`
  allocating mid-probe can trigger a GC, but the probed container is a root, its entries are children
  (traced), and nothing moves — so cached `const Entry&` references stay valid; nested mutation is
  separately rejected.
- alloc() runs the GC BEFORE inserting the new object, and callers that build a container off-arena
  root its fresh children via RootScope before alloc (GetIter, apply, sort, extend, remove/count/index
  snapshots) — the documented rooting rule is followed at the sites I read.
- pool size-class round-trip: classOf(n) is identical on allocate and (sized) deallocate; the virtual
  ~Object guarantees the complete-object size reaches operator delete. Consistent.
- CardTable forEachDirtyRange clamp is off-by-one-clean (`lo >= count` skip, `hi > count` clamp).
