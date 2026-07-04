#!/usr/bin/env python3
# Cross-language benchmark driver: Kirito vs C++ vs Python vs Lua 5.1 vs Bash.
#
# For each workload it runs the equivalent implementations (compare_bench.{cpp,py,ki,lua,sh}), each of
# which times its internal repetitions (excluding process startup and input generation) and prints
# "<mean_ns> <stddev_ns>". This driver tabulates the RAW per-repetition mean ± stddev for every
# language — no ratios — with every cell in a row rendered in the SAME unit (chosen from the row's
# slowest language) to 3 significant figures. Workloads are grouped into:
#   pessimistic  — interpreter-bound tight loops, where the bytecode VM pays per-op dispatch
#   optimistic   — work delegated to library/builtins, where per-op overhead is amortized
#
# Usage:  python3 tests/bench/compare.py [--ki PATH] [--lua PATH] [--scale F]
#   --scale F     multiply every reps count by F (e.g. 0.1 for a quick smoke run)
#   --ki PATH     path to the `ki` interpreter (default: ../../build/ki relative to this script)
import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# (name, category, N, reps).  N/reps are tuned so each Kirito run stays in a few seconds while every
# language times the same algorithm on the same LCG-generated data. (Bash uses adaptive reps — it
# repeats only until ~0.5 s has elapsed — since a full run would take many minutes.)
WORKLOADS = [
    ("sum_loop",   "pessimistic", 1000, 2000),
    ("fib",        "pessimistic",   17,  300),
    ("sieve",      "pessimistic", 1500,  500),
    ("sort",       "optimistic",  1500, 2000),
    ("dict_ops",   "optimistic",  1000, 1500),
    ("string_ops", "optimistic",  1500, 2000),
]

# Fixed column order for the table.
LANGS = ["C++ (-O2)", "Python 3", "Lua 5.1", "Bash", "Kirito"]


def unit_of(ns):
    """The single time unit for every row: microseconds."""
    del ns
    return ("us", 1e3)


def sig3(x):
    """Format x to 3 significant figures (fixed-point)."""
    if x <= 0:
        return "0.00"
    dec = max(0, 2 - int(math.floor(math.log10(x))))
    return "%.*f" % (dec, x)


def cell(v, factor):
    """A 'mean ± stddev' cell in the row's unit, or an em-dash when the language is absent."""
    if not v:
        return "—"
    return "%s ± %s" % (sig3(v[0] / factor), sig3(v[1] / factor))


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("command failed: %s\n%s" % (" ".join(cmd), p.stderr))
    mean, sd = p.stdout.strip().split("\n")[-1].split()
    return float(mean), float(sd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ki", default=os.path.join(HERE, "..", "..", "build", "ki"))
    ap.add_argument("--lua", default="lua5.1", help="Lua 5.1 interpreter (skipped if not found)")
    ap.add_argument("--scale", type=float, default=1.0)
    args = ap.parse_args()

    ki = os.path.abspath(args.ki)
    if not os.path.exists(ki):
        sys.exit("ki interpreter not found at %s (build it first, or pass --ki)" % ki)

    # Lua 5.1 is optional: include it only when the interpreter is on PATH (or given explicitly).
    lua = args.lua if os.path.exists(args.lua) else shutil.which(args.lua)
    if not lua:
        print("note: %s not found — skipping the Lua column" % args.lua, flush=True)

    # Build the C++ baseline (-O2) into a temp binary.
    cbin = os.path.join(tempfile.gettempdir(), "kirito_compare_bench")
    print("compiling C++ baseline (-O2) ...", flush=True)
    subprocess.run(["g++", "-O2", "-std=c++17", os.path.join(HERE, "compare_bench.cpp"), "-o", cbin],
                   check=True)

    pybench = os.path.join(HERE, "compare_bench.py")
    kibench = os.path.join(HERE, "compare_bench.ki")
    lubench = os.path.join(HERE, "compare_bench.lua")
    shbench = os.path.join(HERE, "compare_bench.sh")

    rows = []
    for name, cat, N, reps in WORKLOADS:
        reps = max(20, int(reps * args.scale))
        print("running %-12s (N=%d, reps=%d) ..." % (name, N, reps), flush=True)
        cpp = run([cbin, name, str(N), str(reps)])
        py = run([sys.executable, pybench, name, str(N), str(reps)])
        lu = run([lua, lubench, name, str(N), str(reps)]) if lua else None
        sh = run(["bash", shbench, name, str(N), str(reps)])
        kj = run([ki, kibench, name, str(N), str(reps)])
        rows.append((name, cat, N, reps, [cpp, py, lu, sh, kj]))

    # --- present results: raw per-rep mean ± stddev, one unit per row, 3 significant figures ---
    W = 20  # per-language cell width
    hdr = "%-12s %5s %6s %5s  %s" % (
        "workload", "N", "reps", "unit", "  ".join("%-*s" % (W, l) for l in LANGS))
    bar = "=" * len(hdr)
    print()
    print(bar)
    print("  Raw per-repetition timing — mean ± stddev, one unit per row  (lower is better)")
    print(bar)
    for cat in ("pessimistic", "optimistic"):
        print("\n[%s]" % cat)
        print(hdr)
        print("-" * len(hdr))
        for name, c, N, reps, vals in rows:
            if c != cat:
                continue
            means = [v[0] for v in vals if v]
            unit, factor = unit_of(max(means))
            cells = "  ".join("%-*s" % (W, cell(v, factor)) for v in vals)
            print("%-12s %5d %6d %5s  %s" % (name, N, reps, unit, cells))
    print("\n  (Bash uses adaptive reps — ~0.5 s per workload, min 5 — so its stddev is over fewer"
          "\n   samples; every other language runs the full `reps` shown.)\n")


if __name__ == "__main__":
    main()
