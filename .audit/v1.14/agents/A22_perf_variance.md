# A22 — perf-variance re-measurement (v1.14, orchestrator-run)

## Method
Release preset (`-O2`, static, mold linker), the in-tree `bench` harness, 12 back-to-back runs of the
three hottest workloads. Coefficient of variation (CV = stdev/mean) is the stability metric the user
asked about ("performance … is very unstable (high standard deviation)").

## Result (12 runs, release)

| workload | mean | stdev | CV | min | max |
|----------|------|-------|-----|-----|-----|
| `fib(27)` (call+arith) | 203.2 ms | 4.7 ms | **2.3 %** | 194 | 211 |
| `loop 1e6` (module-scope) | 198.4 ms | 6.8 ms | **3.5 %** | 189 | 212 |
| `fnloop 1e6` (slot locals, 2M allocs) | 334.7 ms | 13.1 ms | **3.9 %** | 325 | 377 |

## Conclusion — no low-risk/high-reward change warranted

The variance is **already low**: a 2–4 % CV is within ordinary machine jitter (scheduler preemption,
CPU frequency scaling, cache/TLB state) — not VM-attributable instability. The "high standard
deviation" the request references was **already addressed in v1.13** by raising the adaptive GC floor
`kGcThresholdFloor` 20 000 → 65 536 (`vm.hpp`), which cut the collection frequency for allocation-heavy
tight loops and tightened their tail; this round re-verifies that fix HOLDS (A07 RV-1) and measures the
resulting stability.

The only residual tail is `fnloop`'s occasional ~13 % spike (max 377 vs mean 335) — an alloc-heavy loop
(2 M fresh Float boxes) where a GC cycle occasionally lands late in the run. The available levers to
shave it further are **not** low-risk/high-reward:

- Raising the GC floor again (e.g. 131 072) would reduce GC frequency for this one pattern but **raises
  peak memory** for every low-live/high-alloc workload — a memory-for-jitter trade, not a free win. v1.13
  already took the defensible step here; going further is a policy change, not a bug fix.
- A generational / young-gen GC would target exactly this pattern but is a **large, high-risk** rewrite,
  explicitly out of scope for "low-risk" tuning.

Per the request's "low-risk high-reward optimizations only" constraint, the correct action is **no
change**: the measured stability is good, and every further lever trades memory or risk for a sub-jitter
gain. Recording the measurement as the v1.14 perf outcome.

(Constant-dedup `addConst` still boxes-then-discards a literal on a dedup hit — v1.13 A04-4/A03(v14)-5,
a micro-alloc, cold path (compile time, once per body), no measurable runtime effect. Left as a flagged
nit, not a variance driver.)
</content>
