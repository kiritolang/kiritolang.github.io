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
| recursion | `fib(30)` | 327.2 ± 2.84 | 416.8 ± 4.15 | 45.40 ± 0.81 | 62.78 ± 0.37 |
| recursion | `ackermann(3,6)` | 41.89 ± 0.45 | 47.16 ± 0.77 | 3.20 ± 0.03 | 19.81 ± 1.06 |
| loops | `sum_loop` | 146.1 ± 3.89 | 192.3 ± 1.69 | 13.22 ± 0.21 | 46.73 ± 1.11 |
| loops | `float_loop` | 262.4 ± 3.11 | 337.4 ± 1.49 | 16.86 ± 0.62 | 61.74 ± 5.66 |
| loops | `nested_loop` | 49.12 ± 0.79 | 66.47 ± 0.64 | 4.29 ± 0.15 | 16.49 ± 0.58 |
| loops | `collatz` | 442.8 ± 1.52 | 471.8 ± 2.00 | 78.80 ± 2.77 | 104.9 ± 0.94 |
| loops | `gcd_loop` | 187.7 ± 1.22 | 195.7 ± 3.67 | 22.54 ± 0.24 | 39.76 ± 0.31 |
| algorithms | `sieve` | 649.4 ± 7.46 | 715.9 ± 5.91 | 58.86 ± 1.27 | 107.2 ± 0.80 |
| algorithms | `quicksort` | 1280 ± 30.43 | 1261 ± 16.62 | 417.6 ± 1.59 | 193.6 ± 1.41 |
| algorithms | `matmul_manual` | 332.6 ± 4.99 | 392.9 ± 3.87 | 43.59 ± 0.45 | 84.59 ± 1.18 |
| lists | `list_build` | 455.1 ± 6.10 | 462.7 ± 2.22 | 53.06 ± 1.67 | 40.54 ± 0.48 |
| lists | `list_sum` | 751.7 ± 5.52 | 922.6 ± 13.57 | 139.5 ± 1.05 | 285.2 ± 2.93 |
| lists | `list_sort` | 932.2 ± 30.95 | 931.8 ± 3.09 | 215.4 ± 2.01 | 249.9 ± 2.89 |
| dicts/sets | `dict_build` | 207.9 ± 1.01 | 261.6 ± 4.44 | 54.80 ± 0.34 | 76.16 ± 7.35 |
| dicts/sets | `dict_lookup_int` | 156.7 ± 1.79 | 189.8 ± 0.95 | 15.92 ± 0.24 | 58.99 ± 0.40 |
| dicts/sets | `dict_lookup_str` | 360.7 ± 5.11 | 379.5 ± 1.33 | 206.6 ± 1.51 | 146.2 ± 2.66 |
| dicts/sets | `set_ops` | 391.5 ± 2.25 | 438.8 ± 3.51 | 34.50 ± 0.30 | 76.78 ± 2.03 |
| strings | `str_concat` | 140.3 ± 0.82 | 149.3 ± 0.58 | 34.08 ± 1.24 | 22.26 ± 0.50 |
| strings | `str_split_join` | 95.72 ± 0.74 | 94.53 ± 0.61 | 101.3 ± 0.54 | 18.06 ± 0.56 |
| strings | `str_search` | 62.61 ± 0.33 | 62.07 ± 0.33 | 83.89 ± 0.37 | 6.08 ± 0.04 |
| OO | `method_call` | 138.9 ± 1.66 | 203.0 ± 1.40 | 20.95 ± 0.22 | 28.63 ± 0.21 |
| OO | `attr_rw` | 89.06 ± 0.32 | 101.9 ± 0.69 | 13.59 ± 0.22 | 26.97 ± 5.88 |
| OO | `object_create` | 175.4 ± 2.93 | 187.5 ± 2.45 | 31.75 ± 0.42 | 34.91 ± 1.27 |
| OO | `poly_dispatch` | 74.49 ± 0.74 | 112.4 ± 1.37 | 10.95 ± 0.29 | 16.15 ± 0.13 |
| functional | `map_filter` | 675.8 ± 2.17 | 733.6 ± 2.96 | 6.54 ± 1.48 | 12.51 ± 0.26 |

## Class scaling (fixed attribute/method access, N members)

Access cost is O(1) in every runtime — flat from 1000 to 10000 members.

| workload | N | Kirito 1.18.0 | Kirito 1.17.1 | Lua 5.1 | Python 3 |
|---|---|---|---|---|---|
| `class_attr` (400k reads) | 1000 | 56.78 ± 1.40 | 68.00 ± 0.45 | 8.51 ± 0.04 | 31.33 ± 1.66 |
| `class_attr` (400k reads) | 10000 | 58.64 ± 0.83 | 70.17 ± 0.34 | 11.99 ± 0.41 | 34.13 ± 2.55 |
| `class_method` (400k calls) | 1000 | 132.4 ± 1.81 | 204.2 ± 1.00 | 18.31 ± 0.14 | 46.45 ± 1.85 |
| `class_method` (400k calls) | 10000 | 135.9 ± 1.10 | 211.3 ± 2.19 | 19.63 ± 0.18 | 49.75 ± 3.13 |

## Numeric / stdlib workloads (Kirito only)

These exercise Kirito-native features (tensors, the regex engine, primality) with no direct Python/Lua
equivalent, so only the two Kirito versions are shown (default GC cadence).

| workload | Kirito 1.18.0 | Kirito 1.17.1 |
|---|---|---|
| `tensor_add` (20k elems, ×800) | 66.54 ± 0.19 | 92.25 ± 0.92 |
| `tensor_sum` (20k elems, ×2000) | 23.89 ± 0.05 | 24.34 ± 0.05 |
| `tensor_slice` (100×100, ×1500) | 48.56 ± 5.35 | 182.9 ± 7.38 |
| `tensor_matmul` (64×64, ×150) | 10.97 ± 0.78 | 8.06 ± 0.95 |
| `regex_match` (one-shot, ×30000) | 74.66 ± 1.02 | 81.99 ± 1.38 |
| `isprime` (2‥199, ×20) | 0.45 ± 0.01 | 0.49 ± 0.01 |
| `isprimeaks` (2‥199, ×20) † | 1215 ± 62 | — |

† `isprime` is the deterministic **trial-division** test in both versions (near parity above).
`isprimeaks` — new in 1.18.0 — is the deterministic **AKS** test: exact and polynomial-time, but
orders of magnitude slower (≈2700× here) and practical only for small `n`. Prefer `isprime` or the
probabilistic `isprobableprime` when speed matters; use `isprimeaks` only when a deterministic
polynomial-time witness is specifically required. (`—`: `isprimeaks` did not exist in 1.17.1.) See the
[`int` module](#int).

## Compile-time optimizations

These are transparent — they change **speed, not results** — so they need no code changes on your part.

- **Cheap calls.** A function call reuses a pooled operand-stack buffer (no per-call allocation), pushes
  interned chunk-name indices instead of copying strings, and reads its compiled body straight off the
  AST node (no per-call cache lookup). Every call — recursion, a `map`/`filter` callback, a method —
  benefits.
- **Function inlining.** A single-`return` lambda — called directly (`(Function(x): return …)(a)`),
  passed as a `map`/`filter` callback, or bound to an immutable local helper (`var sq = Function(x):
  return x * x` then `sq(i)`) — is compiled straight into the caller, with no call frame or scope
  allocation. It may read its own parameters, globals, and **variables it captures** from an enclosing
  scope (those are read in place at the call site, so a closure over a changing variable stays correct).
  Inlining is hygienic — the inlined body never sees the caller's *other* locals and its parameters
  never leak out — so a program's results are identical to a normal call. One visible consequence: an
  inlined call does not appear as its own frame in a traceback (including `sys.traceback()`); use
  `--no-inline` when you need every call to show a frame.
  - **`InlineFunction` — an explicit inline you can rely on.** Automatic `Function` inlining above is
    *transparent*: the compiler decides, and whether it happens changes speed, not results. `InlineFunction`
    is the opposite in one respect — it is an **assertion by you** that a call inlines, and unlike the
    transparent optimization it **can fail at compile time**: if the call cannot be inlined (it is
    recursive, a value-use, a class method, multi-statement, keyword-called, …) that is a loud compile
    error, not a silent normal call. Same runtime behaviour as `Function` when it does inline; the
    difference is the guarantee. See the [language guide](02-language-guide.md#functions). `--no-inline`
    demotes every `InlineFunction` back to a plain `Function` (guarantee and restrictions both lifted).
- **Combinator fusion.** A `for x in map(f, xs):` or `for x in filter(p, xs):` loop whose callback is an
  inline-eligible lambda literal is fused into a single loop that inlines the callback per element — no
  intermediate `map`/`filter` view and no per-element call. Laziness/semantics are unchanged; a shadowed
  builtin or a non-literal callback keeps the ordinary (lazy) path.

Run `ki --no-inline` (or set `KIRITO_NO_INLINE`) to disable the inlining transform for debugging — it
restores per-call traceback frames and never changes a program's output. The language rules that keep
inlining sound (notably: a closure may read a captured variable and mutate a captured *container*, but
may not rebind a captured variable) are always enforced, with or without the flag.
