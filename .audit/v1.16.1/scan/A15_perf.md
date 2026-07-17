# A15 — Performance-variance sweep (v1.16.1)

Goal: find SOURCES of run-to-run timing variance (high stddev), propose ONLY low-risk / high-reward
mitigations. Analysis + proposals only — NO source edits.

## Measured variance (build-debug/ki, -O0, compare_bench.ki, 5 process launches each)

Per-rep mean/stddev (ns) reported by the harness itself (30 reps in-process):

| workload | N | mean (ns) | stddev (ns) | CV |
|---|---|---|---|---|
| sum_loop | 200000 | ~490M | ~1.7M | **0.35%** (stable) |
| sort | 20000 | ~27.7M | 0.1–0.38M | **0.4–1.4%** (stable) |
| dict_ops | 20000 | ~54.9M | ~6.8M | **~12.4%** (HIGH) |
| string_ops | 20000 | ~23.8M | ~6.7M | **~28%** (VERY HIGH) |

`--gc-stats` for the same runs (minor/major count + total ms, live/young/capacity):

| workload | minor | minor ms | major | major ms | ~ms/major | live | capacity |
|---|---|---|---|---|---|---|---|
| dict_ops | 33 | 212.5 | 8 | 132.3 | **16.5** | 117567 | 142023 |
| string_ops | 19 | 104.8 | 5 | 75.8 | **15.2** | 80915 | 141944 |
| sort | 3 | 15.1 | 1 | 8.9 | 8.9 | 28831 | 47516 |
| sum_loop | 567 | 1856 | 189 | 580 | 3.1 | 15280 | 33503 |

**Root cause of the variance is confirmed: MAJOR-collection pauses that are large relative to a
single rep AND infrequent enough not to average out.** dict_ops: a 16.5 ms major on a 55 ms rep is
+30%, and only 8 of 30 reps carry one -> bimodal per-rep timing -> CV 12%. string_ops: a 15 ms major
on a 24 ms rep is +63%, 5 of 30 reps -> CV 28%. sum_loop, by contrast, does 189 tiny (~3 ms) majors
spread over 30 reps (~6/rep) so GC cost is baked uniformly into every rep -> CV 0.35%. sort does a
single major -> one outlier rep, otherwise flat.

The absolute numbers are -O0 (no release build present — must not build); a release build shrinks
every number but the STRUCTURE (bimodal majors -> variance) is O-level-independent.

## Validation experiment: raising the GC cadence makes variance WORSE

`KIRITO_GC_THRESHOLD` pins minor=n and major=n*8 (couples both cadences). Same 30-rep runs:

| workload | threshold | mean (ns) | stddev (ns) | minors (ms) | majors (ms) | capacity |
|---|---|---|---|---|---|---|
| dict_ops | default | 54.7M | 7.0M | 33 (213) | 8 (134) | 142023 |
| dict_ops | 131072 | 50.0M | **10.7M** | 9 (169) | 1 (33) | 291827 |
| dict_ops | 262144 | 49.7M | **14.5M** | 5 (172) | 0 | 351391 |
| string_ops | default | 23.7M | 6.6M | 19 (105) | 5 (76) | 141944 |
| string_ops | 262144 | 22.7M | **12.7M** | 3 (108) | 0 | 307563 |

**Counter-intuitive but decisive: raising the (coupled) threshold shaved a little off the MEAN
(fewer total collections) but roughly DOUBLED the stddev.** Reason: enlarging the nursery makes each
minor a big pause and drops the collection COUNT to ~3-9 over 30 reps, so each surviving collection
lands on a small, random subset of reps -> the per-rep distribution becomes sharply **bimodal**
(GC-reps vs clean-reps), which is exactly what inflates stddev. Note string_ops at threshold 262144
has **zero majors** yet stddev 12.7M — so the variance is not "majors" per se, it is **any large
collection that is rare relative to the rep count**. Conversely sum_loop's ~750 tiny collections over
30 reps bake a uniform GC slice into every rep -> CV 0.35%.

**Conclusion / guiding principle for this audit:** per-rep timing is BIMODAL, not high-variance
Gaussian. The small default nursery is already the right choice for uniformity — the variance is the
handful of reps that eat a collection whose cost is large vs a rep. Two levers reduce it without
regressing correctness or big-O: (a) make the *summary metric* robust to bimodality, and (b) reduce
the number/size of the rare large collections **without enlarging the frequent-minor nursery** (the
coupled knob above does the opposite and backfires). Everything else (page-fault first-touch on the
ballooning arena capacity, vector reallocations, non-stationary warm-up cadence) is secondary.

## Findings

### P15-1 [risk: Low] [reward: High] The harness reports mean+population-stddev over a BIMODAL distribution — switch to median + p95/MAD
- where: `tests/bench/compare_bench.{ki,cpp,py,sh,lua}` (all print `<mean_ns> <stddev_ns>`); the driver `compare_bench.py`/reporting
- Variance source: NOT the runtime — the *metric*. Per-rep time is bimodal (a rep either eats a GC
  collection or does not). With 8 of 30 reps carrying a 16 ms major, population stddev ~= sqrt(p(1-p))*spike
  and is dominated by a handful of reps; it makes throughput "look" unstable even though the typical
  (median) rep is rock-steady. The `.ki` harness even computes stddev in-band, so the headline number
  the maintainer sees is the bimodal one.
- Proposed fix: also print **median** and **p95** (or median ± MAD). Sort `samples`, take the middle
  and the 0.95 index — a few lines, no new deps, in every language port. Keep mean/stddev too, but
  read variance off median-vs-p95 (the "clean rep" vs "worst rep" gap), which is what actually matters.
- Expected effect / measure: median CV for dict_ops/string_ops drops to the sub-1% region (the clean
  reps), and p95 exposes the GC tail explicitly. Zero runtime risk — reporting only. This is the single
  highest-reward, lowest-risk item: it reframes "unstable perf" as "a quantified GC tail".

### P15-2 [risk: Med] [reward: High] Decouple the MAJOR threshold from the nursery and raise ONLY the major multiplier
- where: `src/kirito/vm.hpp:206` (`gcThreshold_ = std::max(kGcThresholdFloor, live * 4)`), triggered at `vm.hpp:73`
- Variance source: the rare, expensive MAJOR pauses (dict_ops 16.5 ms each, 8 over 30 reps) are the
  outlier reps. In ADAPTIVE mode `minorThreshold_` is already fixed at 32768 and independent of
  `gcThreshold_`, so the major cadence can be loosened WITHOUT touching the frequent-minor nursery —
  unlike `KIRITO_GC_THRESHOLD`, which scales both and (measured above) doubles stddev.
- Proposed fix: raise the major factor (e.g. `live * 4` -> `live * 6`/`* 8`) and/or the floor, keeping
  `kMinorNursery` untouched. Fewer majors over the same run -> fewer outlier reps, while minors stay
  small/frequent/uniform (the low-variance regime). Also cuts sum_loop's pure-waste majors (see P15-3).
- Expected effect / how to measure: build release, run `compare_bench.ki` with `--gc-stats`; expect
  dict_ops/string_ops major count to fall (8->~3, 5->~2) with the mean flat-to-better and, crucially,
  the nursery-driven minor uniformity preserved (unlike the coupled env knob). Big-O unchanged (major
  stays O(live), just spaced further). **Risk (why Med, not Low):** delayed reclamation of old-spanning
  dead cycles -> higher peak arena capacity between majors; the floor must stay a comfortable multiple
  of the nursery (already documented). MUST be validated by a measured sweep (`tools/tests/bench`) —
  I could not build here (concurrent-build ban), so this is proposed, not proven. Treat as the primary
  runtime lever but gate it on a real before/after measurement.

### P15-3 [risk: Low] [reward: Med] The major floor forces ~189 near-empty majors on a small stable live set (sum_loop) — pure wasted work
- where: `src/kirito/vm.hpp:206` + floor `kGcThresholdFloor = 131072` (`vm.hpp:544`)
- Variance source: mostly a THROUGHPUT leak, but relevant: sum_loop's live set is a stable 15 280, yet
  it runs 189 majors totalling 580 ms (~23% of its 2.5 s runtime) that reclaim essentially nothing a
  minor did not already get (live never grows). Because live*4 (61 120) < floor, majors fire on the
  floor regardless of how little is reclaimable.
- Proposed fix (low-risk variant of P15-2): raise `kGcThresholdFloor` to a larger multiple of the
  nursery (e.g. 4x = 131072 is already 4x; try 8x = 262144) so a small-live workload spaces majors
  further; the nursery is untouched so minor uniformity holds. Optionally skip promoting a scheduled
  collection to a major when the *previous* major freed < a few % (a one-int ratio check on the sweep
  return already available at `vm.hpp:195`) — a cheap "don't bother" gate, no new data structures.
- Expected effect / measure: `--gc-stats` sum_loop major count and major-ms drop sharply; mean falls
  a few %. Correctness/big-O unaffected (majors are a superset-reclaim; doing fewer only delays freeing
  old garbage, never frees a live object). Risk: slightly higher steady arena for pathological cases.

### P15-4 [risk: Low] [reward: Low-Med] Every MINOR walks the operand stacks TWICE, with a per-slot arena deref for the IterCursor rescan
- where: `src/kirito/vm.hpp:235-244` (the `gcNeedsRootRescan` loop) vs the same regions already scanned
  in `forEachRoot` at `vm.hpp:218`/`174`
- Variance source: throughput, not variance (uniform per minor) — but the v1.16.0 aux-root rescan the
  brief flagged. For EVERY handle in EVERY live operand-stack region, each minor does
  `arena_.deref(h)` (a random-access slot lookup = likely cache miss) just to call the virtual
  `gcNeedsRootRescan()`, which is `false` for everything except `IterCursor` (the ONLY overrider,
  `bytecode_vm.hpp:55`). On minor-heavy workloads (sum_loop: 567 minors) this is a second O(stack)
  cache-miss stream per minor for almost no work.
- Proposed fix: track whether any live IterCursor exists (increment/decrement a `size_t
  liveIterCursors_` in IterCursor ctor/dtor, or push live cursors onto a tiny VM-side vector) and skip
  the whole loop when zero. Common case (no lazy cursor mid-flight) pays nothing; correctness identical.
- Expected effect / measure: instruction-count / minor-ms drop on minor-heavy loops; measure via
  `--gc-stats` minor-ms and the bench mean. Big-O unchanged. Low risk (a counter + an early `continue`).

### P15-5 [risk: Low] [reward: Med] Arena/pool grow geometrically to a large workload-specific high-water; first-touch page faults + vector reallocations land in random early reps (non-stationary warm-up)
- where: `src/kirito/arena.hpp:35-38` (`slots_`/`young_` reserve `kInitialReserve = 8192`, arena.hpp:203);
  capacity reaches 142k (default) / up to 351k (raised threshold). `pool.hpp` free-lists are lazily
  first-touched. Harness warms up only ONE rep (`compare_bench.ki:100`).
- Variance source: two secondary contributors. (1) The single harness warm-up rep does not reach the
  steady-state high-water (dict_ops capacity climbs across several TIMED reps as `live` grows), so
  those reps eat a `slots_` reallocation (copying up to ~142k `Slot`s = moving that many unique_ptrs)
  plus first-touch page faults — landing unpredictably. (2) The adaptive major threshold retargets to
  `live*4` after each major, and `live` grows over the first reps, so the collection cadence is
  NON-STATIONARY early -> the head of the sample distribution has a different GC rate than the tail,
  inflating stddev beyond the steady-state value.
- Proposed fix (both harness-side, zero core risk): (a) increase warm-up to ~3-5 reps (or "warm until
  `capacity` stops growing") so timing starts at steady state; (b) optionally raise `kInitialReserve`
  or add an opt-in `vm.reserveArena(n)` the bench calls before timing to pre-touch to high-water. Pure
  capacity/warm-up — no semantic effect (the arena comment already notes the same rationale for the
  8192 reserve, S2).
- Expected effect / measure: the early-rep transients disappear; median unchanged, stddev on the
  builtin-heavy workloads drops (the non-GC reps stop carrying reallocation spikes). Compare p95/stddev
  before/after with the same seed. Cannot regress correctness (reserve is pure capacity).

### P15-6 [risk: Low] [reward: Low] Per-call operand stack reserves only 16 — loops with many temporaries reallocate mid-run
- where: `src/kirito/bytecode_vm.hpp:68` (`stack_.reserve(16)`)
- Variance source: a frame whose operand stack exceeds 16 entries reallocates its `Handle` buffer
  during execution (the vector OBJECT is stable so the auxRoots_ pointer stays valid — correctness is
  fine); the reallocation is a small one-off spike. Minor and mostly per-frame-deterministic, so a low
  contributor, but a needless one on wide expressions / deep call frames.
- Proposed fix: bump the initial reserve modestly (e.g. 32) — trivially cheap since it is a fresh
  vector per frame and most frames stay small. Measure by frame-heavy microbench; do NOT over-reserve
  (fib allocates one per call, so keep it small). Low risk, low reward — include only if touching this file.

## Out of scope (flagged, NOT proposed as low-risk)

- **Tenuring threshold (`kGcOldAge == 1`).** The structural cause of the majors is that a per-rep
  transient larger than the nursery survives ONE minor mid-build and is promoted to OLD, becoming old
  garbage that only a major reclaims. A classic fix is to require surviving 2+ minors before tenuring
  (so build-and-drop-within-2-cycles transients die in a minor, and majors become rare). This would
  attack the variance at its root, but it changes the young-set size, minor cost, and the sweepYoung /
  card-marking / remembered-set invariants (all currently assume single-step promotion). **Med-High
  risk / a real GC redesign — explicitly out of scope for this low-risk audit.** Noted as the strategic
  direction if a future round wants to tackle GC-pause variance head-on.
- **Denormals / FP flush-to-zero:** N/A — the benchmark workloads are integer/string; no hot float
  path. (The stddev math itself is trivial and off the timed path.)
- Anything trading correctness or an algorithm's big-O for speed (e.g. capping the major threshold,
  which the design comment at vm.hpp:199-203 explicitly rejects as re-introducing quadratic behaviour).
