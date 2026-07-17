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

## Findings

