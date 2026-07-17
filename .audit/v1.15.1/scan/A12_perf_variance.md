# A12 — Performance: TIMING STABILITY (variance), round v1.15.1

Scope: the user's ask — "performance isn't terrible, but very unstable (high standard deviation).
Low-risk high-reward optimizations only." Metric: **intra-run per-rep CV = stddev/mean** from
`tools/tests/bench/compare_bench.ki` (whole-run wall time averages jitter away — the v1.14 mistake).

Analysis-only: no builds, no commits, no edits.

## Blocker (logged immediately)

`build-release/ki` **does not exist** (only `build-debug/ki`, `-O0`). Any timing claim needs the
release binary. Requested from the main agent; doing read-only analysis + debug-binary
`--gc-stats` counter work meanwhile.

## Log

- Started. Reading v1.15 FINDINGS "Perf — variance analysis" + GC/pool sources.

## Read-only leads (to be confirmed by measurement)

### L1 — every GC allocates and frees its mark-stack from scratch
`vm.hpp:184-187` (`collectGarbage`) and `vm.hpp:216-219` (`minorCollect`) each declare
`std::vector<Handle> work;` + `std::vector<Handle> childbuf;` as **function locals**. They are
malloc'd, geometrically grown, and freed **on every single collection**.

At `kMinorNursery = 32768`, `work` reaches O(surviving young) entries × 8 B/Handle → **~256 KB**,
grown by ~17 doublings from zero each time. glibc's `M_MMAP_THRESHOLD` starts at 128 KB, so a
buffer that size is served by **mmap + munmap per collection**, with a fresh page-fault storm on
first touch. That is a textbook jitter source and it is paid on *every minor*.
Fix: hoist both to reused members (`clear()` keeps capacity). Zero semantic change.
Reentrancy check needed: minor/major must never nest (they don't — `children()` cannot allocate).

### L2 — every major makes a second full O(capacity) pass just to count
`vm.hpp:206`: `gcThreshold_ = max(kGcThresholdFloor, arena_.liveCount() * 4)`, and
`arena.hpp:156` `liveCount()` walks all of `slots_`. It runs immediately after `sweep()`
(`arena.hpp:96`), which already walks all of `slots_` and already knows the survivor count.
Fix: `sweep()` returns/records the live count. Zero semantic change.

Next: prove L1 with `strace -c -e trace=mmap,munmap` (binary-independent — mmap churn shows up
identically at -O0).

## L3 (THE BIG ONE) — every collection rescans `tempRoots_` IN FULL, so a native that
## accumulates N objects under one `RootScope` is O(N²/nursery)

`--gc-stats`, debug binary, `compare_bench.ki sum_loop 3000000 5`:

```
14942101725.8 2326351959.0
gc-stats: minor=2189 (39419.1 ms)  major=8 (6120.99 ms)  live=3007158  young=6295  capacity=6033410
```

**Minor GC is 39.4 s of a ~74.7 s run (53%)** — and the debug CV is already 2326/14942 = **0.156**
even at -O0, where a fixed pause is diluted ~10x by the slow mutator.

Why `live=3007158` for a workload that keeps one accumulator: **`range` materializes a List**
(`runtime.hpp:3181-3193`, comment "range materializes a List (no lazy generators yet)"), so
`range(3000000)` is 3M live `IntVal`s (i > 256 ⇒ outside the intern range ⇒ one object each).
`run_sum_loop` calls `range(N)` *inside* the timed body, so this is rebuilt every rep.

The quadratic is in the **root scan**, not (as v1.15 assumed) the remembered set. `range` builds a
bare `ListVal` that is **not in the arena until the final `vm.alloc`** (`runtime.hpp:3182-3192`):

```cpp
RootScope rs(vm);
auto list = std::make_unique<ListVal>();
list->elems.reserve(count);
for (uint64_t k = 0; k < count; ++k) {
    list->elems.push_back(rs.add(vm.makeInt(v)));   // <--每 element becomes a tempRoot
    ...
}
return vm.alloc(std::move(list));
```

So during construction all N elements sit in `tempRoots_`, and `forEachRoot` (`vm.hpp:171`,
`for (Handle h : tempRoots_) f(h)`) is walked **in full by every minor and every major**. With a
32768 nursery, building N elements triggers ~N/32768 minors, each scanning an average of N/2 roots
⇒ **O(N²/65536) root visits**. For N=3M that is ~138M visits *per rep*, and none of them can free
anything (every element is live by construction).

This is the same *shape* as the "growing List re-remembers itself on every append" quadratic v1.15
identified — a minor that does O(big) work and reclaims ~nothing — but it reaches it through
`tempRoots_`, and it is not fixed by card marking. `RootScope`-accumulate-in-a-loop is a **pervasive
native idiom** (range, sorted, map, zip, split, join, …), so this is not a `range`-only wart.

**Consequence for the proposal set:** both quadratics share one cure that is far cheaper than card
marking — see P1 (adaptive nursery) below.

## MEASURED — release binary (`build-release/ki`, 1.15.0)

### Baseline, default cadence (per-rep intra-run CV; 5 independent runs each)

| workload (N, reps) | CV per run | median CV |
|---|---|---|
| sum_loop 1000000 7 | 0.086 0.073 0.090 0.063 0.073 | **0.073** |
| dict_ops 200000 7 | 0.194 0.185 0.189 0.184 0.182 | **0.185** |
| sort 300000 7 | 0.044 0.041 0.058 0.077 0.050 | **0.050** |
| string_ops 200000 7 | 0.353 0.370 0.339 0.340 0.340 | **0.340** |

Run-to-run spread is tiny → the variance is **structural (GC), not box noise**, despite the
concurrent audit agents.

### Nursery sweep — `--gc-threshold TH` (pins minor=TH, major=8·TH, adaptivity off)

3 runs per cell; every cell reproduced to ~±0.02 CV. `mean` = per-rep mean.

| TH | sum_loop mean/CV | dict_ops mean/CV | string_ops mean/CV | sort mean/CV |
|---|---|---|---|---|
| 8192 | 1.00 s / **0.024** | 0.194 s / **0.034** | 0.044 s / **0.056** | **0.120 s** / 0.051 |
| 32768 (≈default) | 0.374 s / 0.020 | 0.125 s / 0.109 | 0.024 s / 0.309 | 0.162 s / 0.055 |
| 131072 | 0.234 s / 0.024 | 0.110 s / 0.153 | 0.021 s / 0.451 | 0.258 s / 0.056 |
| 524288 | **0.193 s** / 0.066 | 0.100 s / 0.202 | 0.022 s / 0.480 | 0.269 s / 0.069 |
| 2097152 | 0.203 s / 0.128 | **0.088 s** / 0.492 | 0.033 s / 0.958 | 0.348 s / 0.088 |

**The workloads want opposite nursery sizes, and the reason is survival rate:**

- **sum_loop wants a BIG nursery** (0.374 s → 0.193 s, **48% faster**, at 524288; and 8192 costs it
  **2.7×**). This is L3 exactly: during `range(N)` construction *every* young object survives, so a
  minor is pure waste, and its cost is O(accumulated roots). Fewer minors ⇒ less quadratic.
- **sort wants a SMALL nursery** (0.120 s at 8192 vs 0.269 s at 524288 — **2.2× faster**): `a.copy()`
  churn dies wholesale, so a minor is pure profit and a big nursery just inflates the arena.
- **dict_ops / string_ops**: throughput mildly prefers big, **CV strongly prefers small**
  (string_ops **0.340 → 0.056, a 6× stabilisation**; dict_ops **0.185 → 0.034, 5.4×**).

### The headline conclusion

**At TH=8192 every workload's CV is excellent simultaneously** — sum_loop 0.024, dict_ops 0.034,
string_ops 0.056, sort 0.051 (vs the default 0.073 / 0.185 / 0.340 / 0.050) — **and sort is 25%
faster**. The *only* thing that makes 8192 unaffordable is sum_loop's 2.7× regression, and that
regression is **entirely the L3 quadratic**, not an intrinsic cost of a small nursery.

So v1.15's "32768 is the measured knee" is real but is an **artifact of the L3/remembered-set
quadratics**. Remove the quadratic and the knee moves down, and the small-nursery CV column becomes
available for free.
