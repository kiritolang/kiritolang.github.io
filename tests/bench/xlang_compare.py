#!/usr/bin/env python3
"""Comprehensive cross-language benchmark: Kirito 1.17.1 vs Kirito 1.18.0 vs Python 3 vs Lua 5.1.

Runs a broad workload set (recursion, loops, containers, strings, OO, functional, algorithms) plus
class attribute/method SCALING at 1000 and 10000 members. Each (workload, runtime) cell is measured
over >= 10 runs and reported as mean +/- sample stddev (milliseconds). The 1.17.1 -> 1.18.0 speedup is
computed from the means.

Usage:
    python3 tests/bench/xlang_compare.py \
        --old dist/ki-linux-x64 --new build-bin/ki-release \
        [--python python3] [--lua lua] [--runs 10]
"""
import argparse
import math
import os
import shutil
import statistics
import subprocess
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
XL = os.path.join(HERE, "xlang")

FIXED = [
    ("recursion", ["fib", "ackermann"]),
    ("loops", ["sum_loop", "float_loop", "nested_loop", "collatz", "gcd_loop"]),
    ("algorithms", ["sieve", "quicksort", "matmul_manual"]),
    ("lists", ["list_build", "list_sum", "list_sort"]),
    ("dicts/sets", ["dict_build", "dict_lookup_int", "dict_lookup_str", "set_ops"]),
    ("strings", ["str_concat", "str_split_join", "str_search"]),
    ("OO", ["method_call", "attr_rw", "object_create", "poly_dispatch"]),
    ("functional", ["map_filter"]),
]
SCALE_NS = [1000, 10000]
COLW = 17  # column width for "mean±std" cells


def r(v):
    """Reasonable resolution: ~4 significant figures scaled by magnitude."""
    if v >= 1000:
        return f"{v:.0f}"
    if v >= 100:
        return f"{v:.1f}"
    if v >= 10:
        return f"{v:.2f}"
    return f"{v:.3f}"


def cell(vals):
    if not vals:
        return "-"
    m = statistics.fmean(vals)
    sd = statistics.stdev(vals) if len(vals) > 1 else 0.0
    return f"{r(m)}±{r(sd)}"


def mean_of(vals):
    return statistics.fmean(vals) if vals else None


def run_cmd(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if p.returncode != 0:
        return None
    return p.stdout


def samples_fixed(cmd, runs):
    """Run `runs` times, return the list of millisecond samples (last stdout line = ms)."""
    out = []
    for _ in range(runs):
        s = run_cmd(cmd)
        if s is None:
            return []
        try:
            out.append(float(s.strip().splitlines()[-1]))
        except (ValueError, IndexError):
            return []
    return out


def samples_scaling(cmd, runs):
    """Run `runs` times; return {result_name: [ms samples]} parsed from RESULT lines."""
    agg = {}
    for _ in range(runs):
        s = run_cmd(cmd)
        if s is None:
            break
        for line in s.splitlines():
            p = line.split("\t")
            if len(p) == 3 and p[0] == "RESULT":
                agg.setdefault(p[1], []).append(float(p[2]))
    return agg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--python", default="python3")
    ap.add_argument("--lua", default="lua")
    ap.add_argument("--runs", type=int, default=10, help="runs per cell (>=10); mean+/-stddev reported")
    args = ap.parse_args()
    runs = max(10, args.runs)

    lua = shutil.which(args.lua) or args.lua
    py = shutil.which(args.python) or args.python

    def ver(ki):
        try:
            return subprocess.run([ki, "--version"], capture_output=True, text=True).stdout.strip()
        except Exception:
            return ki

    rts = [
        ("1.17.1", lambda wl: [args.old, os.path.join(XL, "bench.ki"), wl]),
        ("1.18.0", lambda wl: [args.new, os.path.join(XL, "bench.ki"), wl]),
        ("Python3", lambda wl: [py, os.path.join(XL, "bench.py"), wl]),
        ("Lua5.1", lambda wl: [lua, os.path.join(XL, "bench.lua"), wl]),
    ]
    cols = [k for k, _ in rts]

    print(f"Kirito OLD : {args.old}   [{ver(args.old)}]")
    print(f"Kirito NEW : {args.new}   [{ver(args.new)}]")
    print(f"Python     : {py}")
    print(f"Lua        : {lua}")
    print(f"runs/cell  : {runs}   started {time.strftime('%Y-%m-%d %H:%M:%S')}")
    print("\nCells are mean±stddev in milliseconds (lower is better). "
          "Last column = 1.17.1/1.18.0 speedup.\n")

    hdr = f"{'workload':<18}" + "".join(f"{c:>{COLW}}" for c in cols) + f"{'1.17->1.18':>12}"

    speedups = []
    for group, wls in FIXED:
        print(f"--- {group} ---")
        print(hdr)
        for wl in wls:
            samp = {k: samples_fixed(mk(wl), runs) for k, mk in rts}
            row = f"{wl:<18}" + "".join(f"{cell(samp[c]):>{COLW}}" for c in cols)
            o, n = mean_of(samp["1.17.1"]), mean_of(samp["1.18.0"])
            if o and n and n > 0:
                speedups.append((wl, o / n))
                row += f"{('%.2fx' % (o / n)):>12}"
            else:
                row += f"{'-':>12}"
            print(row)
        print()

    # ---- class attribute/method scaling ----
    print("--- class scaling (N attributes + N methods; time a fixed access) ---")
    print(hdr)
    tmp = tempfile.mkdtemp(prefix="kirito_scale_")

    def gen_ki(n):
        L = ["var io = import(\"io\")", "var time = import(\"time\")", "class Big:",
             "    var _init_ = Function(self):"]
        L += [f"        self.attr{i} = {i}" for i in range(n)]
        L += [f"    var m{i} = Function(self): return {i}" for i in range(n)]
        L += ["var obj = Big()", "var t0 = time.monotonic()", "var s = 0", "var i = 0",
              "while i < 400000:", f"    s = s + obj.attr0 + obj.attr{n-1}", "    i = i + 1",
              f"io.print(\"RESULT\\tclass_attr_{n}\\t\" + format((time.monotonic()-t0)*1000.0, \".3f\"))",
              "t0 = time.monotonic()", "s = 0", "i = 0",
              "while i < 400000:", f"    s = s + obj.m0() + obj.m{n-1}()", "    i = i + 1",
              f"io.print(\"RESULT\\tclass_method_{n}\\t\" + format((time.monotonic()-t0)*1000.0, \".3f\"))",
              "discard s"]
        return "\n".join(L) + "\n"

    def gen_py(n):
        L = ["import time", "class Big:", "    def __init__(self):"]
        L += [f"        self.attr{i} = {i}" for i in range(n)]
        L += [f"    def m{i}(self): return {i}" for i in range(n)]
        L += ["obj = Big()", "t0 = time.perf_counter()", "s = 0", "i = 0",
              "while i < 400000:", f"    s = s + obj.attr0 + obj.attr{n-1}", "    i = i + 1",
              f"print('RESULT\\tclass_attr_{n}\\t%.3f' % ((time.perf_counter()-t0)*1000.0))",
              "t0 = time.perf_counter()", "s = 0", "i = 0",
              "while i < 400000:", f"    s = s + obj.m0() + obj.m{n-1}()", "    i = i + 1",
              f"print('RESULT\\tclass_method_{n}\\t%.3f' % ((time.perf_counter()-t0)*1000.0))"]
        return "\n".join(L) + "\n"

    def gen_lua(n):
        L = ["local Big = {}", "Big.__index = Big", "function Big.new()",
             "  local self = setmetatable({}, Big)"]
        L += [f"  self.attr{i} = {i}" for i in range(n)]
        L += ["  return self", "end"]
        L += [f"function Big:m{i}() return {i} end" for i in range(n)]
        L += ["local obj = Big.new()", "local t0 = os.clock()", "local s = 0", "local i = 0",
              "while i < 400000 do", f"  s = s + obj.attr0 + obj.attr{n-1}", "  i = i + 1", "end",
              f"io.write(string.format('RESULT\\tclass_attr_{n}\\t%.3f\\n', (os.clock()-t0)*1000.0))",
              "t0 = os.clock()", "s = 0", "i = 0",
              "while i < 400000 do", f"  s = s + obj:m0() + obj:m{n-1}()", "  i = i + 1", "end",
              f"io.write(string.format('RESULT\\tclass_method_{n}\\t%.3f\\n', (os.clock()-t0)*1000.0))"]
        return "\n".join(L) + "\n"

    scale = {}  # name -> {col: [samples]}
    for n in SCALE_NS:
        fki = os.path.join(tmp, f"s_{n}.ki"); open(fki, "w").write(gen_ki(n))
        fpy = os.path.join(tmp, f"s_{n}.py"); open(fpy, "w").write(gen_py(n))
        flua = os.path.join(tmp, f"s_{n}.lua"); open(flua, "w").write(gen_lua(n))
        for col, cmd in [("1.17.1", [args.old, fki]), ("1.18.0", [args.new, fki]),
                         ("Python3", [py, fpy]), ("Lua5.1", [lua, flua])]:
            for nm, vals in samples_scaling(cmd, runs).items():
                scale.setdefault(nm, {})[col] = vals

    for nm in sorted(scale, key=lambda s: (s.split("_")[1], int(s.split("_")[-1]))):
        d = scale[nm]
        row = f"{nm:<18}" + "".join(f"{cell(d.get(c, [])):>{COLW}}" for c in cols)
        o, n2 = mean_of(d.get("1.17.1", [])), mean_of(d.get("1.18.0", []))
        if o and n2 and n2 > 0:
            speedups.append((nm, o / n2))
            row += f"{('%.2fx' % (o / n2)):>12}"
        else:
            row += f"{'-':>12}"
        print(row)
    shutil.rmtree(tmp, ignore_errors=True)

    if speedups:
        gm = math.exp(sum(math.log(s) for _, s in speedups if s > 0) / len(speedups))
        best = max(speedups, key=lambda x: x[1])
        worst = min(speedups, key=lambda x: x[1])
        print(f"\nGeometric-mean 1.17.1 -> 1.18.0 speedup across {len(speedups)} workloads: {gm:.2f}x")
        print(f"  best:  {best[0]} {best[1]:.2f}x     worst: {worst[0]} {worst[1]:.2f}x")


if __name__ == "__main__":
    main()
