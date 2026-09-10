# Bonus 07 — Performance reference

Measured wall-clock timings for Kirito **1.18.0** across a broad micro-benchmark suite. These are the
raw timer results — **mean ± sample standard deviation over 10 runs** — for one fixed amount of work
per workload; they are *not* a comparison against other versions or languages, just a reference for
the relative cost of common operations.

**Methodology.** Each row runs a single self-contained workload (fixed iteration counts) and times it
with `time.monotonic()`. Every cell is the best-effort mean ± stddev of 10 runs on the reference
machine; absolute numbers depend on hardware, so treat them as *ratios between workloads*, not as
portable constants. The exact workloads live in `tests/bench/xlang/bench.ki` and
`tests/bench/churn_bench.ki`, and are reproducible with `tests/bench/xlang_compare.py` /
`tests/bench/churn_compare.py`.

<!--norun-->
## General workloads

| category | workload | time (ms) |
|---|---|---|
| recursion | `fib(30)` | 391.2 ± 18.60 |
| recursion | `ackermann(3, 6)` | 47.24 ± 0.78 |
| loops | `sum_loop` (2M int adds) | 149.4 ± 2.29 |
| loops | `float_loop` (2M float adds) | 268.1 ± 7.80 |
| loops | `nested_loop` (700×700) | 50.65 ± 1.50 |
| loops | `collatz` (1‥30000) | 441.3 ± 3.10 |
| loops | `gcd_loop` (200k euclid) | 189.0 ± 1.57 |
| algorithms | `sieve` (300× to 5000) | 630.5 ± 9.07 |
| algorithms | `quicksort` (300× 1000) | 1248 ± 13.67 |
| algorithms | `matmul_manual` (40³, ×30) | 324.4 ± 4.12 |
| lists | `list_build` (2M appends) | 445.5 ± 6.75 |
| lists | `list_sum` (2000× 10k) | 791.4 ± 8.21 |
| lists | `list_sort` (2000× 1000) | 888.4 ± 6.24 |
| dicts/sets | `dict_build` (2M int inserts) | 207.9 ± 2.35 |
| dicts/sets | `dict_lookup_int` (1.5M) | 149.7 ± 1.25 |
| dicts/sets | `dict_lookup_str` (1.5M) | 347.6 ± 2.48 |
| dicts/sets | `set_ops` (add + membership) | 382.3 ± 1.64 |
| strings | `str_concat` (3000× 300) | 138.6 ± 1.27 |
| strings | `str_split_join` (500×) | 93.73 ± 0.45 |
| strings | `str_search` (`count`, 1000×) | 61.86 ± 0.69 |
| OO | `method_call` (500k) | 144.1 ± 2.28 |
| OO | `attr_rw` (500k) | 85.90 ± 0.62 |
| OO | `object_create` (300k) | 173.3 ± 1.40 |
| OO | `poly_dispatch` (300× 1000) | 79.37 ± 1.56 |
| functional | `map_filter` (100× 2000) | 745.1 ± 20.19 |

## Class scaling (fixed attribute/method access, N members)

Access cost is O(1) — flat as the member count grows from 1000 to 10000.

| workload | N = 1000 (ms) | N = 10000 (ms) |
|---|---|---|
| `class_attr` (400k reads) | 56.79 ± 4.38 | 59.59 ± 1.51 |
| `class_method` (400k calls) | 147.9 ± 3.86 | 157.7 ± 2.15 |

## Numeric / stdlib workloads (default GC cadence)

| workload | time (ms) |
|---|---|
| `tensor_add` (20k elems, ×800) | 66.34 ± 1.03 |
| `tensor_sum` (20k elems, ×2000) | 22.91 ± 0.36 |
| `tensor_slice` (100×100, ×1500) | 44.66 ± 2.65 |
| `tensor_matmul` (64×64, ×150) | 8.13 ± 1.00 |
| `regex_match` (one-shot, ×30000) | 79.64 ± 6.61 |
| `isprime_small` (AKS, 2‥199, ×20) † | 1078 ± 17.23 |

† `isprime` is the deterministic **AKS** test — exact and polynomial-time, but far slower than the
probabilistic `isprobableprime`. Use `isprobableprime` when speed matters; see the
[`int` module](#int).
