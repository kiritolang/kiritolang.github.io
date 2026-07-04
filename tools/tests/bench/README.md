# Benchmarks

Two things live here:

- `bench.cpp` — the in-tree correctness/timing harness run by CTest (a feature isn't "fast enough"
  until this stays green); built like any other test.
- The **cross-language comparison** (`compare.py` driver + `compare_bench.{cpp,py,ki,lua,sh}`) —
  Kirito vs C++ vs Python vs Lua 5.1 vs Bash on identical algorithms over identical (LCG-generated)
  data. It is a manual tool, not a CTest case (a full run shells out to `g++`/`python3`/`lua5.1`/`bash`).

## Running the comparison

```sh
cmake --build build --target ki          # the driver needs the ki interpreter
python3 tests/bench/compare.py           # full run (reps in the thousands)
python3 tests/bench/compare.py --scale 0.05   # quick smoke run
python3 tests/bench/compare.py --ki /path/to/ki
python3 tests/bench/compare.py --lua /path/to/lua   # Lua interpreter (default: lua5.1 on PATH)
```

Each implementation times its internal repetitions with its own high-resolution clock
(`steady_clock` / `time.perfcounterns` / `os.clock` / bash `EPOCHREALTIME`), excluding process startup
and input generation, then prints `<mean_ns> <stddev_ns>`. The driver tabulates the **raw
per-repetition mean ± stddev** for every language — no slowdown ratios — with every cell in a row
rendered in the **same time unit** (chosen from the row's slowest language) to **3 significant
figures**. Example row:

```
workload         N   reps  unit  C++ (-O2)             Python 3              Lua 5.1               Bash                  Kirito
sum_loop      1000   2000    ms  0.000374 ± 0.0000138  0.0382 ± 0.00512      —                     3.15 ± 0.0912         0.261 ± 0.235
```

The **Lua 5.1** column is included automatically when a `lua5.1` interpreter is on `PATH` (or pass
`--lua`); it is skipped (`—`) if none is found. Lua 5.1 has no integer type (numbers are doubles), so
its 31-bit LCG is computed with an exact 16-bit split-multiply to reproduce the identical sequence,
and it times with `os.clock()` — the best clock stock Lua 5.1 offers.

**Bash** is the same algorithms in pure Bash (`compare_bench.sh`, no external `sort`/`awk` in the
timed region). Because a Bash interpreter is orders of magnitude slower, it uses **adaptive reps** —
it repeats each workload only until ~0.5 s has elapsed (min 5), capped at the requested `reps` — so
its per-repetition mean is comparable while its stddev is over fewer samples. A full run therefore
still finishes in well under a minute even with Bash included.

## Workloads

Chosen to bracket Kirito's performance envelope:

- **pessimistic** — interpreter-bound tight loops where the bytecode VM pays per-operation dispatch on
  every step: `sum_loop` (arithmetic loop), `fib` (recursive calls), `sieve` (nested loops + indexed
  list writes).
- **optimistic** — work delegated to C++ builtins/library so the interpreter overhead is amortized
  over a few native calls: `sort` (builtin sort), `dict_ops` (hash-map insert/lookup), `string_ops`
  (`split`/`join`).

The expected shape: Kirito is orders of magnitude slower than C++/Python on the pessimistic loops,
but closes most of the gap on the optimistic, builtin-delegated workloads — exactly what a
bytecode interpreter with a fast C++ standard library should show.
