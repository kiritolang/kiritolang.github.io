#!/usr/bin/env python3
# Cross-language benchmark driver: Kirito vs C++ vs Python.
#
# For each workload it runs the three equivalent implementations (compare_bench.{cpp,py,ki}), each of
# which times `reps` internal repetitions (excluding process startup and input generation) and prints
# "<mean_ns> <stddev_ns>". This driver tabulates mean ± stddev per language and the Kirito slowdown
# factor, grouped into:
#   pessimistic  — interpreter-bound tight loops, where a tree-walker is expected to do poorly
#   optimistic   — work delegated to library/builtins, where Kirito's per-op overhead is amortized
#
# Usage:  python3 tests/bench/compare.py [--ki PATH] [--scale F] [--reps-min N]
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
# language times the same algorithm on the same LCG-generated data.
WORKLOADS = [
    ("sum_loop",   "pessimistic", 1000, 2000),
    ("fib",        "pessimistic",   17,  300),
    ("sieve",      "pessimistic", 1500,  500),
    ("sort",       "optimistic",  1500, 2000),
    ("dict_ops",   "optimistic",  1000, 1500),
    ("string_ops", "optimistic",  1500, 2000),
]


def fmt(ns):
    if ns < 1e3:
        return "%.0f ns" % ns
    if ns < 1e6:
        return "%.2f us" % (ns / 1e3)
    if ns < 1e9:
        return "%.2f ms" % (ns / 1e6)
    return "%.2f s" % (ns / 1e9)


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

    rows = []
    for name, cat, N, reps in WORKLOADS:
        reps = max(20, int(reps * args.scale))
        print("running %-12s (N=%d, reps=%d) ..." % (name, N, reps), flush=True)
        cpp = run([cbin, name, str(N), str(reps)])
        py = run([sys.executable, pybench, name, str(N), str(reps)])
        lu = run([lua, lubench, name, str(N), str(reps)]) if lua else None
        kj = run([ki, kibench, name, str(N), str(reps)])
        rows.append((name, cat, N, reps, cpp, py, lu, kj))

    # --- present results ---
    def cell(v):
        return "%s ± %s" % (fmt(v[0]), fmt(v[1])) if v else "—"

    print()
    bar = "=" * 116
    print(bar)
    print("  Kirito vs C++ vs Python vs Lua 5.1  —  mean ± stddev per repetition  (lower is better)")
    print(bar)
    hdr = "%-12s %-7s %8s  %18s  %18s  %18s  %18s  %7s %7s %7s" % (
        "workload", "N", "reps", "C++ (-O2)", "Python 3", "Lua 5.1", "Kirito",
        "Ki/C++", "Ki/Py", "Ki/Lua")
    for cat in ("pessimistic", "optimistic"):
        print("\n[%s]" % cat)
        print(hdr)
        print("-" * 116)
        for name, c, N, reps, cpp, py, lu, kj in rows:
            if c != cat:
                continue
            ki_cpp = kj[0] / cpp[0] if cpp[0] else float("inf")
            ki_py = kj[0] / py[0] if py[0] else float("inf")
            ki_lua = "%6.1fx" % (kj[0] / lu[0]) if lu and lu[0] else "     —"
            print("%-12s %-7d %8d  %18s  %18s  %18s  %18s  %6.0fx %6.1fx %s" % (
                name, N, reps, cell(cpp), cell(py), cell(lu), cell(kj), ki_cpp, ki_py, ki_lua))

    # geometric-mean slowdowns
    def geomean(vals):
        return math.exp(sum(math.log(v) for v in vals) / len(vals))
    for cat in ("pessimistic", "optimistic"):
        sub = [r for r in rows if r[1] == cat]
        gm_cpp = geomean([kj[0] / cpp[0] for _, _, _, _, cpp, _, _, kj in sub])
        gm_py = geomean([kj[0] / py[0] for _, _, _, _, _, py, _, kj in sub])
        tail = ""
        if all(lu for _, _, _, _, _, _, lu, _ in sub):
            gm_lua = geomean([kj[0] / lu[0] for _, _, _, _, _, _, lu, kj in sub])
            tail = ",  %.1fx Lua 5.1" % gm_lua
        print("\n  %-11s geo-mean slowdown:  Kirito is %.0fx C++,  %.1fx Python%s" % (
            cat, gm_cpp, gm_py, tail))
    print()


if __name__ == "__main__":
    main()
