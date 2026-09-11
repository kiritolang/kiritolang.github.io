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
| recursion | `fib(30)` | 390.2 ± 6.74 | 413.3 ± 6.67 | 45.52 ± 0.22 | 61.85 ± 0.27 |
| recursion | `ackermann(3,6)` | 47.46 ± 0.98 | 45.87 ± 1.17 | 3.21 ± 0.06 | 18.62 ± 0.38 |
| loops | `sum_loop` | 162.2 ± 6.38 | 188.4 ± 1.08 | 13.22 ± 0.12 | 44.63 ± 0.17 |
| loops | `float_loop` | 265.6 ± 4.43 | 328.8 ± 0.83 | 16.62 ± 0.26 | 61.21 ± 1.37 |
| loops | `nested_loop` | 53.75 ± 1.03 | 67.63 ± 0.60 | 4.25 ± 0.08 | 16.54 ± 0.18 |
| loops | `collatz` | 480.7 ± 1.89 | 475.0 ± 3.68 | 78.38 ± 0.21 | 104.4 ± 0.71 |
| loops | `gcd_loop` | 202.2 ± 1.49 | 191.1 ± 0.50 | 22.60 ± 0.13 | 39.27 ± 0.31 |
| algorithms | `sieve` | 658.5 ± 9.36 | 705.5 ± 15.84 | 59.30 ± 0.32 | 111.4 ± 5.98 |
| algorithms | `quicksort` | 1250 ± 4.48 | 1245 ± 6.50 | 419.8 ± 1.63 | 189.2 ± 1.59 |
| algorithms | `matmul_manual` | 342.2 ± 18.86 | 387.4 ± 5.10 | 43.29 ± 0.23 | 84.24 ± 0.85 |
| lists | `list_build` | 450.5 ± 8.97 | 458.2 ± 4.96 | 52.59 ± 0.95 | 39.97 ± 0.49 |
| lists | `list_sum` | 818.6 ± 9.29 | 904.7 ± 16.78 | 140.5 ± 1.10 | 286.9 ± 11.62 |
| lists | `list_sort` | 905.8 ± 38.78 | 917.6 ± 2.32 | 216.2 ± 0.82 | 248.7 ± 3.64 |
| dicts/sets | `dict_build` | 224.1 ± 17.39 | 263.2 ± 2.42 | 55.83 ± 0.51 | 74.14 ± 0.40 |
| dicts/sets | `dict_lookup_int` | 157.4 ± 0.48 | 188.6 ± 0.86 | 16.02 ± 0.15 | 58.56 ± 0.70 |
| dicts/sets | `dict_lookup_str` | 359.4 ± 2.72 | 371.8 ± 3.17 | 206.9 ± 0.46 | 146.7 ± 5.31 |
| dicts/sets | `set_ops` | 399.9 ± 2.94 | 428.1 ± 5.11 | 34.69 ± 0.11 | 78.10 ± 1.34 |
| strings | `str_concat` | 146.3 ± 0.49 | 152.4 ± 0.48 | 33.81 ± 0.29 | 21.82 ± 0.31 |
| strings | `str_split_join` | 98.13 ± 0.54 | 97.61 ± 1.51 | 103.8 ± 7.85 | 18.11 ± 0.04 |
| strings | `str_search` | 63.32 ± 0.73 | 62.89 ± 0.69 | 84.80 ± 0.55 | 5.92 ± 0.02 |
| OO | `method_call` | 148.3 ± 2.23 | 199.4 ± 0.61 | 21.58 ± 1.32 | 28.77 ± 0.43 |
| OO | `attr_rw` | 100.5 ± 2.21 | 101.0 ± 1.85 | 13.90 ± 0.29 | 24.77 ± 4.73 |
| OO | `object_create` | 182.4 ± 2.44 | 189.8 ± 0.85 | 32.78 ± 2.53 | 33.92 ± 1.08 |
| OO | `poly_dispatch` | 81.72 ± 2.16 | 113.4 ± 2.70 | 11.15 ± 0.28 | 15.90 ± 0.83 |
| functional | `map_filter` | 694.0 ± 5.68 | 724.6 ± 16.09 | 6.11 ± 0.37 | 12.62 ± 0.14 |

## Class scaling (fixed attribute/method access, N members)

Access cost is O(1) in every runtime — flat from 1000 to 10000 members.

| workload | N | Kirito 1.18.0 | Kirito 1.17.1 | Lua 5.1 | Python 3 |
|---|---|---|---|---|---|
| `class_attr` (400k reads) | 1000 | 57.87 ± 6.92 | 67.35 ± 0.23 | 8.62 ± 0.10 | 30.62 ± 2.11 |
| `class_attr` (400k reads) | 10000 | 57.51 ± 0.63 | 69.44 ± 0.33 | 12.09 ± 0.31 | 32.34 ± 0.77 |
| `class_method` (400k calls) | 1000 | 153.3 ± 6.66 | 206.3 ± 7.03 | 18.63 ± 0.25 | 45.78 ± 2.42 |
| `class_method` (400k calls) | 10000 | 153.0 ± 3.71 | 208.4 ± 1.73 | 19.86 ± 0.49 | 48.82 ± 2.85 |

## Numeric / stdlib workloads (Kirito only)

These exercise Kirito-native features (tensors, the regex engine, primality) with no direct Python/Lua
equivalent, so only the two Kirito versions are shown (default GC cadence).

| workload | Kirito 1.18.0 | Kirito 1.17.1 |
|---|---|---|
| `tensor_add` (20k elems, ×800) | 68.68 ± 0.41 | 94.25 ± 1.38 |
| `tensor_sum` (20k elems, ×2000) | 23.36 ± 0.08 | 24.03 ± 0.35 |
| `tensor_slice` (100×100, ×1500) | 43.79 ± 1.96 | 177.8 ± 3.84 |
| `tensor_matmul` (64×64, ×150) | 7.87 ± 0.77 | 8.08 ± 0.91 |
| `regex_match` (one-shot, ×30000) | 77.65 ± 2.89 | 83.45 ± 3.34 |
| `isprime` (2‥199, ×20) | 0.46 ± 0.01 | 0.50 ± 0.01 |
| `isprimeaks` (2‥199, ×20) † | 1075 ± 2 | — |

† `isprime` is the deterministic **trial-division** test in both versions (near parity above).
`isprimeaks` — new in 1.18.0 — is the deterministic **AKS** test: exact and polynomial-time, but
orders of magnitude slower (≈2300× here) and practical only for small `n`. Prefer `isprime` or the
probabilistic `isprobableprime` when speed matters; use `isprimeaks` only when a deterministic
polynomial-time witness is specifically required. (`—`: `isprimeaks` did not exist in 1.17.1.) See the
[`int` module](#int).
