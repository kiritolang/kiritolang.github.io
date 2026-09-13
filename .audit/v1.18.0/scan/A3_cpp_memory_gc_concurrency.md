# A3 — C++ core: memory / UB / GC / concurrency

Probed on build-asan/ki (ASan+UBSan) with KIRITO_GC_THRESHOLD=1 (forced GC per alloc) using fresh,
non-interned values. **No CONFIRMED bugs.** Every reentrancy/lifetime hazard already has an explicit,
correct defense; every adversarial reproducer ran clean.

## VERIFIED CORRECT
- Arena alloc/sweep/promotion: non-moving generational mark-sweep; slot generation bump + permanent
  retirement at UINT32_MAX (ABA guard); gen-0 null sentinel.
- Generational write barrier + remembered set + card table: wholesale `resetRemembered()` guarded by
  `static_assert(kGcOldAge == 1)`; `CardTable::forEachDirtyRange` clamps to live child count.
- `gcNeedsRootRescan`: only overrider is the stack-only `IterCursor` (never heap-stored); minorCollect
  rescans the auxRoots operand stacks for it. No heap-resident holder returns true.
- Lazy iterators (Map/Filter/Zip/Enumerate/Range/Iter): children() covers held handles; transient
  SourceCursor buffers rooted per allocating next(). Clean under nested map(filter(map(...))) + GC=1.
- Autograd TensorVal (parent Handles in a plain std::vector inside shared_ptr, unbarriered): parents
  populated only at construction of a fresh young tensor inside a RootScope, never mutated on a
  promoted tensor; children() covers parents so young-trace promotes in lockstep. Deep-graph clean.
- List sort/remove snapshot into a RootScope before user key/_eq_/_lt_, re-fetch to write back.
- Dict/Set reentrancy: a ProbeScope flag rejects any nested mutation during a _hash_/_eq_/_str_ probe
  → catchable "changed size during comparison", never a UAF.
- Parallel dispatcher: workers are separate VMs/arenas; values cross only as serialized blobs through
  a thread-safe Queue; TaskVal is non-serializable (single-owner); join/shutdown race resolved by
  `joining.exchange(true)` + release/acquire on `done`. No specific race to hand to TSan.

## Informational
`ValueKind::Array` is a reserved enum with no producer; the `|| Array` guards beside `List` are dead
but harmless.
