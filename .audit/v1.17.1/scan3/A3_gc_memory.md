# A3 — GC / MEMORY / UB — scan3

Subsystem: precise generational (non-moving, mark-sweep) GC and all memory/UB safety.
Files read in full: `object.hpp`, `arena.hpp`, `vm.hpp`, `handle.hpp`, the write barriers +
lazy-sequence machinery in `runtime.hpp`, `bytecode_vm.hpp` (IterCursor), and every native
Handle-holder (`collections.hpp` List/Dict/Set, `environment.hpp`, `class_value.hpp` Instance/
BoundMethod, `module.hpp`, `function.hpp`, `stdlib_tensor.hpp` autograd, `stdlib_regex.hpp`
MatchVal, `stdlib_net.hpp` Response, the map/filter/zip/enumerate/iter combinators + SourceCursor).

Method: every probe run on the prebuilt **Address+UBSan** binary `build-bin/ki-asan` (races on
`build-bin/ki-tsan`) with **`KIRITO_GC_THRESHOLD=1`** (collect on every allocation, so every write
barrier and every promotion boundary is exercised) and **fresh non-interned values** (large ints
`> 256`, freshly-built strings, freshly-built lists) so a swept child produces a detectable wrong
value or an asan trap rather than being masked by an interned small-int singleton. Each probe
churns hard (grows/mutates containers, allocates a lot), holds a reference across the churn, then
reads the held reference back and asserts its contents.

## CONFIRMED bugs

**None.** Zero HIGH, zero MED, zero LOW. No asan/UBSan trap and no wrong value was produced by any
probe. The known bug class (an object buffering young Handles in plain C++ storage that is swept
after promotion) is correctly defended: `IterCursor`/`SourceCursor` flag `gcNeedsRootRescan()` and
the minor collector re-traces every such stack-resident old object every minor
(`vm.hpp:237-246`), and each lazy combinator's `next()` roots its un-consumed source buffer via
`SourceCursor::rootInto` before every allocating step. All probes designed to break exactly this
path (s6/s9/s19/s20) passed with correct values.

## SUSPECTED / informational (not counted — no user-code repro)

- `stdlib_net.hpp:776,813` — `Response`/`Session` hold `headersH`/`cookiesH` and correctly report
  them from `children()`. I could not reach these holders from a soak loop without live network
  I/O, so I did not independently soak them; the pattern (two immutable child handles set at
  construction, returned from `children()`) matches the barrier-correct holders. **SUSPECTED-OK,
  unproven** — no repro either way, so nothing to count.

## Verified CORRECT (soak-tested, barrier-correct under threshold=1 + fresh values)

All probes below produced correct values with no ASan/UBSan report. Probe files in `/tmp`.

| # | Holder / path | file:line | Probe (churn `.ki`) | Result |
|---|---|---|---|---|
| s1 | `ListVal` append + `setElem` into promoted-old list; card write barrier | `collections.hpp:54-59,72-75` | `s1_list.ki` — 500 held large-ints, 5000 churn allocs, overwrite each held slot with fresh young, read back | 500/500 correct |
| s2 | `DictVal` set/overwrite into old dict; card barrier | `collections.hpp:229-240,301-304` | `s2_dict.ki` — 400 fresh-string keys / large-int vals, 6000 churn, overwrite held, read back | 400/400 correct |
| s3 | `SetVal` add fresh young into old set | `collections.hpp:457,546-549` | `s3_set.ki` — 400+200 fresh members, 6000 churn | 600/600 retained |
| s4 | `InstanceValue` `setAttr` fresh young into old instance | `class_value.hpp:70-79` | `s4_inst.ki` — 300 held instances, mutate `.v` with fresh young under 6000 churn | 300/300 correct |
| s5 | `EnvValue` closure captures (factory-per-binding) | `environment.hpp:33-71` | `s5_closure.ki` — 300 closures over fresh captures, 6000 churn, call back | 300/300 correct |
| s6 | Lazy `map`/`filter`/`enumerate` held + partially consumed under churn | `runtime.hpp:3363-3499` | `s6_iter.ki` | correct sums |
| s7 | User generator (`_iter_`/`_next_`) yielding fresh values, churn each step | `class_value.hpp` + generator protocol | `s7_gen.ki` — 300 steps, churn per step | total correct |
| s8 | Tensor **autograd node** parents held across churn, then `backward()` | `stdlib_tensor.hpp:83-84,708-745` | `s8_tensor.ki` — 50 leaf tensors, graph built under churn | all grads == [1,1] |
| s9 | `SourceCursor` buffered **fresh-string** source, cursor promoted old mid-loop | `runtime.hpp:3199-3248` | `s9_strmap.ki` — map over 900-char fresh string, churn each step | 900/900 correct |
| s10 | Index-shifting `insert(0)`/`pop(0)` → `allDirty` card path, fresh young at front | `collections.hpp` (CardTable.markAll) | `s10_shift.ki` — 3000 churn w/ front insert+pop | no dangling, sum stable |
| s11 | Large-container **card spill** (>8192 entries, high-index fresh young writes, cards ≥64 word) | `object.hpp:91-133` (CardTable.spill) | `s11_spill.ki` — 20k-elem list, write fresh young at high indices | 0 corrupt |
| s12 | **Parallel** worker VMs churning under GC (race probe) | `stdlib_parallel.hpp`, dispatcher | `s12_par.ki` on **ki-tsan** | no tsan race, total correct |
| s13 | **Multi-axis tensor slices** held across churn (newest code) | `stdlib_tensor.hpp` slice path | `s13_slice.ki` — 15 held `m[i:i+3,2:5]` slices | 15/15 correct |
| s14 | Regex `MatchVal` (holds `subject`) held across churn | `stdlib_regex.hpp:55` | `s14_regex.ki` — 200 held matches over fresh subjects | 200/200 groups correct |
| s15 | Dict deletion/compaction + fresh young re-add into old dict | `collections.hpp` remove/compact | `s15_dictdel.ki` | 250 kept + 250 new all correct |
| s16 | Cyclic `a<->b` + self-cycles, then forced major collection | `arena.hpp:110-128` sweep | `s16_cycle.ki` — 2000 cycles + 50k major-forcing churn | no crash, no leak-abort |
| s17 | **BoundMethod** (captures instance) held across churn | `class_value.hpp:187` | `s17_bound.ki` — 300 held bound methods | 300/300 correct |
| s18 | `sorted(key=fn)` where key allocates heavily (snapshot rooting via `RootScope::addAll`) | `vm.hpp:622` | `s18_sortkey.ki` | correct order/len |
| s19 | `zip` of two fresh strings — every column buffer rooted across pulls | `runtime.hpp:3427-3444` | `s19_zipstr.ki` | 300/300 correct |
| s21 | `collections.deque` both-end append/pop churn (List-backed) | kimodule | `s21_deque.ki` | correct len/total |
| s22 | Tensor slice UB adversarial: empty (`3:1`), reversed (`::-1`), negative start, fully OOB (`100:200`), ellipsis+index | `stdlib_tensor.hpp` slice | `s22_sliceub.ki` | correct shapes/values, no UBSan |
| s23 | Tensor slice-**assignment** then churn, read back | `stdlib_tensor.hpp` setItem | `s23_setslice.ki` | correct, no OOB |

### GC design invariants re-checked against source (read, then exercised)

- Write barrier early-out (`runtime.hpp:64-80`): young/remembered containers skip; only an OLD
  container gaining a YOUNG value enrols. Card form does NOT early-out on `gcRemembered()` before
  marking (a later write to a farther card in the same cycle is not lost) — exercised by s11.
- `resetRemembered()` wholesale clear is guarded by `static_assert(kGcOldAge == 1)`
  (`arena.hpp:83`): promote-on-first-survival means every old→young edge became old→old after a
  minor. Sound as written.
- Generation wrap (`arena.hpp:191-196`): a slot at `UINT32_MAX` is retired (leaked, off free-list)
  rather than wrapping to the reserved gen 0 — no ABA re-validation of a stale handle. Unreachable
  to soak in practice (needs 2^32 reuses); logic verified by read only.
- `minorCollect` auxRoot rescan (`vm.hpp:237-246`) skips null (`generation==0`) handles and only
  re-traces old objects flagging `gcNeedsRootRescan()` — bounded to IterCursor. Exercised by s9
  (cursor demonstrably promotes to old under threshold=1 yet its fresh-char buffer survives).

## Bottom line

Deep, adversarial soak of every user-reachable Handle-holder under the most aggressive collector
setting with un-masked (fresh, non-interned) values found **no memory-safety, UB, or GC-correctness
bug**. This subsystem is barrier-correct on every path I could drive from user code. The only
un-soaked holder is `net.Response` (needs live network); its shape matches the proven-correct
holders but I mark it SUSPECTED-OK rather than CONFIRMED since I have no repro.
