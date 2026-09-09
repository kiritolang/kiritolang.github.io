# A3 — Memory-safety / GC / object-model audit (v1.17.1)

Scope: `arena.hpp`, `object.hpp`, `handle.hpp`, `pool.hpp`, `value.hpp`, `native.hpp`,
`module.hpp`, `class_value.hpp` (read fully), plus every other native `Object` subclass that
stores `Handle`s (`collections.hpp`, `environment.hpp`, `function.hpp`, `bytecode_vm.hpp`
`IterCursor`, the lazy sequences in `runtime.hpp`, and the stdlib value types).

Method: **no compilation.** Reproduction used the prebuilt `build-bin/ki-asan` and
`build-bin/ki-tsan`, `ulimit -s 262144`, and `KIRITO_GC_THRESHOLD=1` (collect on every
allocation; minor threshold 1, major every 8). All probes used **fresh, non-interned** values
(strings `"x"+String(i)`, ints `i+1000..1000000`, fresh String sources whose `iterate()`
allocates fresh chars) per the interned-small-int masking hazard. Probes run both with
`ASAN_OPTIONS=detect_leaks=0` (functional) and `detect_leaks=1` (leak hunt).

## Findings

**NONE.** No confirmed or suspected memory-safety defect was found. Every soak below ran
clean (exit 0, zero AddressSanitizer/UBSan/tsan diagnostics, zero `dangling handle` throws,
zero leaks under `detect_leaks=1`). I tried hard to trap the GC and could not.

This is an honest null, not an unexercised area: the GC design is internally consistent and
the two historically dangerous classes (v1.15 A19 unrooted fresh allocations; the lazy-iterator
non-barriered young-buffer class) are both correctly closed. Details of *why* each Handle-holder
is safe are in the "Verified CORRECT" list.

## GC design as verified (for the next auditor)

- Non-moving generational mark-sweep. `ObjectArena` owns every `Object` via a slot's
  `unique_ptr`; handles carry a generation, bumped on free (`freeSlot`, arena.hpp:187), so a
  stale handle is caught (`at()` throws `dangling handle (stale generation)`). Generation 0 is
  the reserved `Handle{}` sentinel; slot retired at `UINT32_MAX` to avoid ABA (arena.hpp:191).
- Minor (`KiritoVM::minorCollect`, vm.hpp:215) traces only young objects, seeded from
  `forEachRoot` + the remembered set (`childrenInDirtyCards`, dirty-card rescan) + a bounded
  rescan of `gcNeedsRootRescan()` objects on the operand stacks. Major (`collectGarbage`,
  vm.hpp:183) is a full scan-everything, promotes survivors, drops stale cards.
- Write barrier (`gcWriteBarrier`, runtime.hpp:64/73) fires inside every core mutator
  (Env/List/Dict/Set/Instance/Module/Class). The card-aware form marks the written entry's card
  and does **not** early-out on `gcRemembered()` before marking (runtime.hpp:73) — correct for a
  container remembered at one index then written at a far index. Old→young only; young containers
  early-out on one byte test.
- `resetRemembered` wholesale-clears the remembered set + cards after a minor; this is guarded by
  `static_assert(Object::kGcOldAge == 1)` (arena.hpp:83) so raising tenuring can't silently make
  it unsound. Verified the assert matches promote-on-first-survival everywhere (`sweepYoung`
  promotes every survivor at kGcOldAge==1).

## Reproducers run (all CLEAN)

Env for every run: `KIRITO_GC_THRESHOLD=1`, `ulimit -s 262144`, `build-bin/ki-asan` unless noted.

1. `soak1_lazy.ki` — map/filter over a fresh **String** source with an allocating body
   (`c+"!"`), zip/enumerate over fresh lists/strings. Exercises the non-barriered young-buffer
   class (`SourceCursor.buf`, `IterCursor.lazy`, `gcNeedsRootRescan`). → 244430, exit 0.
2. `soak2_containers.ki` — 300-iter churn: fresh String keys + non-interned int values into a
   long-lived Dict/Set and a long-lived (promoted) List gaining young child lists; interleaved
   `remove` to drive tombstones + card marking. → clean, `detect_leaks=1` clean.
3. `soak3_closures.ki` — 200 closures capturing fresh strings/lists, invoked after promotion.
   → clean.
4. `soak4_tensor.ki` — 60 iters of autograd (`matmul→sin→exp→sum→backward`) building a
   `TensorGradNode.parents` graph (plain `std::vector<Handle>`) and reading `.grad`. → True.
5. `soak5_gen.ki` — user `_iter_`/`_next_` generator raising `StopIteration`, fresh tag strings.
   → clean.
6. `soak6_userkey.ki` — user class with `_hash_`/`_eq_` as Dict/Set keys; reentrant probe path;
   fresh instances; lookup by a distinct equal probe instance. → 200 200.
7. `soak7_slice.ki` — v1.17.1 basic-indexing: `xs[2:15:2]`, `xs[::-1]`, negative slices, slice
   **assignment** `a[0:2]=[..]`, String slicing, all on fresh lists/strings. → clean.
8. `soak8_regex.ki` — `MatchVal` over fresh subjects; `group()`, `start()`, `groupdict()`
   (`subject` kept alive via `children`, groups are int offsets into it). → matched, clean.
9. `soak9_defaults.ki` — native default-arg paths under GC=1: `hash.sha256`, numeric `.compare`
   (default tolerances), `d.setdefault(k)` (default None). Dangling-default hunt. → clean.
10. `soak10_parallel.ki` — `parallel.spawn` × 4 workers churning fresh strings, `join()`.
    Run under **ki-tsan** (14000, exit 0) and **ki-asan `detect_leaks=1`** (14000, exit 0).
11. `soak11_cardspill.ki` — a promoted (old) List gaining **12000** fresh young String children,
    exercising the CardTable **spill** vector (cards ≥64, entries ≥8192); read-back of indices
    9000–12000 proves survival. → 24000 12000.
12. `soak12_deep.ki` — 50-deep fresh nested lists: `String(node)` cyclic/depth-guarded stringify
    + structural `==` of two independently built deep structures. → clean.

Plus the repo's own golden scripts re-run under `ki-asan` + `KIRITO_GC_THRESHOLD=1` — all
`rc=0`, zero sanitizer hits: `probe_gc_stress`, `audit_adversarial`, `probe_adversarial`,
`probe_adversarial2`, `probe_binary_adversarial`, `audit_tensor`, `cov_string_regex`,
`random_generators`.

## Verified CORRECT — every Handle-holding type checked

For each: does `children()` (or the card `childrenInDirtyCards`) report **all** stored handles,
and is every store either barriered or covered by `gcNeedsRootRescan`?

- **EnvValue** (environment.hpp) — `vars_` (SmallVec of name→Handle) + `parent_`; all in
  `children()`. Every write (`define`/`assignLocal`/`setAt`) is barriered. The inline SmallVec
  storage is not an arena container but is fully enumerated by `children()`, so a major traces it
  and the barrier handles minor; correct.
- **ListVal** (collections.hpp) — `elems`; `children()` full, `childrenInDirtyCards` card-ranged;
  `append/setElem` barrier the exact card, `insertElem`/reorder/`gcReorderShift` `markAll` when
  old (index shift). Correct.
- **DictVal / SetVal** (collections.hpp) — dense insertion-ordered `entries` + tombstones (stable
  indices ⇒ valid CardTable); `children()`/card-rescan skip tombstones (`key.generation==0`).
  `set/add` barrier key **and** value at their entry; `reindex(compact)` `markAll` on shift.
  `ProbeScope` blocks reentrant mutation during a user `_hash_/_eq_` probe (no realloc-UAF).
  Correct.
- **ClassValue** (class_value.hpp) — `methods`, `base`, `closure`; all in `children()`;
  `defineMethod` barriered. **SuperValue** — `instance`+`startClass`, both in `children()`.
- **InstanceValue** (class_value.hpp) — `cls` + `attrs`; all in `children()`; `setAttr` barriered
  (runtime.hpp). Dunder cache flags are plain bools. Correct.
- **ModuleValue** (module.hpp) — `members`; `children()` full; `setMember`/`setAttr` barriered.
- **KiFunction** (function.hpp) — `closure_` + `ownerClass`(if hasOwner); both in `children()`.
- **NativeFunction** (function.hpp) — `captures_` + each `sig_[i].defaultValue`(hasDefault); both
  in `children()`. Default handles are rooted at build time (`ModuleBuilder::fn` RootScope,
  native.hpp:118; `toleranceSig`, native.hpp:185) so a GC during construction can't sweep them.
- **IterCursor** (bytecode_vm.hpp) — `items` + (lazy) `source` + `lazy->roots()`; `children()`
  full. **This is the one `gcNeedsRootRescan()==true` type** (when `lazy`). Verified it is
  stack-only (created by GetIter, lives on the frame operand stack, never exposed to Kirito / never
  stored in a heap container) — satisfying the documented invariant that the minor rescan only
  walks operand stacks. Nested lazy chains (map(filter(...))) are covered because the outer
  cursor's `roots()` recurses via each `LazyIterator::roots`.
- **Lazy sequences** (runtime.hpp): RangeVal (no handles), MapVal/FilterVal (`fn_`,`src_`),
  ZipVal (`srcs_`), EnumerateVal/IterVal (`src_`) — all report their handles in `children()`.
  Their `LazyIterator`s (`SourceCursor.buf`, `fn`) expose live handles via `roots()`, and each
  combinator's `next()` roots its own un-consumed buffer (`SourceCursor::rootInto`) across the
  allocating call — the fix for the first-`next()`-materialization window. Verified live under
  soak1 with a fresh String source.
- **TensorVal** (stdlib_tensor.hpp) — `node->parents` (plain `std::vector<Handle>`) reported by
  `children()`. Safe without a barrier because the autograd node is built **before** the tensor's
  handle exists and is **never mutated after** the object becomes reachable — so no "old container
  gains young child later" edge is ever created; parent tensors are allocated before the child, so
  they promote in lockstep. Confirmed under soak4.
- **MatchVal** (stdlib_regex.hpp) — only `subject`; groups are int code-point offsets, not
  handles; `children()` reports `subject`. **RegexVal** holds no live handles.
- **ResponseVal / SessionVal** (stdlib_net.hpp) — `headersH`,`cookiesH`; both in `children()`.
- **FileVal / StdStream / BytesIO** — stream state, no persistent arena handle; `LineIter.src`
  is the stream handle, kept alive by the owning IterCursor's `source` / the streamIterate aux
  root; correct.
- **QueueVal / LockVal / EventVal / SemaphoreVal / BarrierVal / TaskVal** (stdlib_parallel.hpp) —
  hold `shared_ptr` to serialized cross-VM primitives, **no live arena handles** (share-nothing),
  so no `children()` needed. Verified under tsan (soak10).
- **Scalars & singletons**: NoneVal/BoolVal/IntVal/FloatVal/StrVal/BytesVal/EllipsisVal —
  leaf values, no handles. **SliceVal** (builtins.hpp) holds start/stop/step and reports them in
  `children()`.
- **BigIntVal / ComplexVal / ComplexMatrixVal / MatrixVal / DateTime / RandomState / TensorGradFlag
  / NoGradCtx** — payloads are numeric/POD; no stored arena handles (bound-method `captures`
  {self} are transient method objects reachable via their own NativeFunction `children()`).

## Notes / residual risk (honest)

- The one place that would break the fast path is adding a **second** `gcNeedsRootRescan()==true`
  type or storing an `IterCursor` on the heap; the minor rescan (vm.hpp:237) walks **only**
  `auxRoots_`, so a heap-resident rescan-type would have its young buffer swept after promotion.
  The invariant is documented (object.hpp:251) but has **no compile-time guard** — a latent
  footgun for a future contributor, not a current defect.
- `pool.hpp` is bypassed under sanitizers, so ASan sees every allocation (good); the release-only
  pool path (size-class free-lists, `FreeLists` TLS drain at thread exit) was **not** exercisable
  under ASan and is out of reach of this method — reviewed by reading only. No issue seen: sized
  delete matches the polymorphic complete-object size, over-aligned types bypass the pool.
