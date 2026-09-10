# Bonus 07 — Performance reference

Measured wall-clock timings for a broad micro-benchmark suite across four runtimes:
**Kirito 1.18.0**, **Kirito 1.17.1**, **Lua 5.1**, and **Python 3**. These are the raw timer results —
**mean ± sample standard deviation over 10 runs** — for one fixed amount of work per workload. Every
runtime runs the *same* algorithm with the *same* iteration counts.

**Methodology.** Each row runs a single self-contained workload (fixed iteration counts) and times it:
Kirito with `time.monotonic()`, Python with `time.perf_counter()` (both wall clock), Lua with
`os.clock()` (CPU time — effectively equal here since the workloads are single-threaded and
CPU-bound). Each cell is the mean ± stddev of 10 runs on one reference machine; absolute numbers are
hardware-dependent, so read them as *ratios between workloads*, not as portable constants. The exact
workloads live in `tests/bench/xlang/bench.{ki,py,lua}` and `tests/bench/churn_bench.ki`, and are
reproducible with `tests/bench/xlang_compare.py` and `tests/bench/churn_compare.py`.

<!--norun-->
## General workloads

All times in milliseconds (mean ± stddev, 10 runs).

| category | workload | Kirito 1.18.0 | Kirito 1.17.1 | Lua 5.1 | Python 3 |
|---|---|---|---|---|---|
| recursion | `fib(30)` | 391.2 ± 18.60 | 412.1 ± 2.58 | 47.02 ± 0.90 | 63.02 ± 3.39 |
| recursion | `ackermann(3,6)` | 47.24 ± 0.78 | 46.98 ± 0.92 | 3.43 ± 0.26 | 18.44 ± 0.38 |
| loops | `sum_loop` | 149.4 ± 2.29 | 190.4 ± 2.50 | 13.49 ± 0.20 | 45.56 ± 2.77 |
| loops | `float_loop` | 268.1 ± 7.80 | 330.6 ± 2.19 | 17.21 ± 0.28 | 58.57 ± 0.58 |
| loops | `nested_loop` | 50.65 ± 1.50 | 66.89 ± 2.08 | 4.31 ± 0.14 | 16.12 ± 0.63 |
| loops | `collatz` | 441.3 ± 3.10 | 457.4 ± 1.67 | 79.34 ± 0.90 | 102.0 ± 0.85 |
| loops | `gcd_loop` | 189.0 ± 1.57 | 189.2 ± 1.08 | 22.95 ± 0.98 | 38.84 ± 0.49 |
| algorithms | `sieve` | 630.5 ± 9.07 | 690.7 ± 3.19 | 59.61 ± 0.40 | 105.3 ± 1.44 |
| algorithms | `quicksort` | 1248 ± 13.67 | 1219 ± 11.02 | 429.1 ± 4.25 | 190.3 ± 3.61 |
| algorithms | `matmul_manual` | 324.4 ± 4.12 | 372.8 ± 1.13 | 43.58 ± 0.39 | 81.99 ± 0.56 |
| lists | `list_build` | 445.5 ± 6.75 | 446.5 ± 2.12 | 53.13 ± 1.55 | 39.53 ± 0.57 |
| lists | `list_sum` | 791.4 ± 8.21 | 899.8 ± 29.59 | 142.7 ± 1.38 | 283.2 ± 23.38 |
| lists | `list_sort` | 888.4 ± 6.24 | 906.4 ± 8.61 | 220.9 ± 2.82 | 249.4 ± 4.02 |
| dicts/sets | `dict_build` | 207.9 ± 2.35 | 256.3 ± 2.56 | 55.98 ± 0.49 | 73.39 ± 2.09 |
| dicts/sets | `dict_lookup_int` | 149.7 ± 1.25 | 180.2 ± 0.46 | 16.12 ± 0.32 | 56.75 ± 2.11 |
| dicts/sets | `dict_lookup_str` | 347.6 ± 2.48 | 365.7 ± 2.23 | 211.6 ± 1.98 | 158.7 ± 46.10 |
| dicts/sets | `set_ops` | 382.3 ± 1.64 | 438.4 ± 16.35 | 35.07 ± 0.42 | 75.30 ± 0.49 |
| strings | `str_concat` | 138.6 ± 1.27 | 146.8 ± 0.61 | 34.49 ± 0.58 | 21.22 ± 0.26 |
| strings | `str_split_join` | 93.73 ± 0.45 | 94.23 ± 1.15 | 102.4 ± 1.11 | 17.66 ± 0.10 |
| strings | `str_search` | 61.86 ± 0.69 | 61.77 ± 1.11 | 85.93 ± 1.40 | 5.78 ± 0.10 |
| OO | `method_call` | 144.1 ± 2.28 | 196.2 ± 1.24 | 21.41 ± 0.27 | 28.16 ± 0.46 |
| OO | `attr_rw` | 85.90 ± 0.62 | 97.81 ± 1.05 | 13.87 ± 0.23 | 22.48 ± 0.34 |
| OO | `object_create` | 173.3 ± 1.40 | 181.9 ± 1.50 | 32.32 ± 0.42 | 33.46 ± 1.02 |
| OO | `poly_dispatch` | 79.37 ± 1.56 | 107.3 ± 1.07 | 11.47 ± 0.52 | 16.83 ± 2.21 |
| functional | `map_filter` | 745.1 ± 20.19 | 762.3 ± 8.17 | 6.13 ± 0.39 | 12.46 ± 0.56 |

## Class scaling (fixed attribute/method access, N members)

Access cost is O(1) in every runtime — flat from 1000 to 10000 members.

| workload | N | Kirito 1.18.0 | Kirito 1.17.1 | Lua 5.1 | Python 3 |
|---|---|---|---|---|---|
| `class_attr` (400k reads) | 1000 | 56.79 ± 4.38 | 66.27 ± 0.86 | 9.03 ± 0.56 | 31.81 ± 3.11 |
| `class_attr` (400k reads) | 10000 | 59.59 ± 1.51 | 70.92 ± 0.99 | 11.58 ± 1.07 | 32.85 ± 1.25 |
| `class_method` (400k calls) | 1000 | 147.9 ± 3.86 | 199.3 ± 1.63 | 21.21 ± 2.49 | 47.64 ± 3.18 |
| `class_method` (400k calls) | 10000 | 157.7 ± 2.15 | 217.0 ± 6.24 | 21.60 ± 2.23 | 49.83 ± 3.62 |

## Numeric / stdlib workloads (Kirito only)

These exercise Kirito-native features (tensors, the regex engine, deterministic AKS primality) with no
direct Python/Lua equivalent, so only the two Kirito versions are shown (default GC cadence).

| workload | Kirito 1.18.0 | Kirito 1.17.1 |
|---|---|---|
| `tensor_add` (20k elems, ×800) | 66.34 ± 1.03 | 92.44 ± 2.34 |
| `tensor_sum` (20k elems, ×2000) | 22.91 ± 0.36 | 24.12 ± 1.32 |
| `tensor_slice` (100×100, ×1500) | 44.66 ± 2.65 | 177.2 ± 4.83 |
| `tensor_matmul` (64×64, ×150) | 8.13 ± 1.00 | 8.88 ± 0.62 |
| `regex_match` (one-shot, ×30000) | 79.64 ± 6.61 | 86.14 ± 5.48 |
| `isprime_small` (2‥199, ×20) † | 1078 ± 17.23 | 0.49 ± 0.01 |

† 1.18.0's `isprime` is the deterministic **AKS** test — exact and polynomial-time, but far slower
than 1.17.1's trial division for small `n` (and than the probabilistic `isprobableprime`, which is
unchanged). Use `isprobableprime` when speed matters; see the [`int` module](#int).
