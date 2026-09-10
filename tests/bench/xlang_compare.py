#!/usr/bin/env python3
"""Comprehensive cross-language benchmark: Kirito 1.17.1 vs Kirito 1.18.0 vs Python 3 vs Lua 5.1.

Runs a broad workload set (recursion, loops, containers, strings, OO, functional, algorithms) plus
class attribute/method SCALING at 1000 and 10000 members. Each (workload, runtime) is run several
times and the best (min) kept. Prints per-workload ms for all four runtimes and the 1.17.1->1.18.0
speedup.

Usage:
    python3 tests/bench/xlang_compare.py \
        --old dist/ki-linux-x64 --new build-bin/ki-release \
        [--python python3] [--lua lua] [--runs 3]
"""
import argparse
import os
import shutil
import subprocess
import sys
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


# ---- class-scaling source generators (N attributes + N methods, time a fixed access) ----
def gen_ki(n):
    L = ["var io = import(\"io\")", "var time = import(\"time\")", "class Big:",
         "    var _init_ = Function(self):"]
    for i in range(n):
        L.append(f"        self.attr{i} = {i}")
    for i in range(n):
        L.append(f"    var m{i} = Function(self): return {i}")
    L += [
        "var obj = Big()",
        "var t0 = time.monotonic()", "var s = 0", "var i = 0",
        "while i < 400000:",
        f"    s = s + obj.attr0 + obj.attr{n-1}",
        "    i = i + 1",
        f"io.print(\"RESULT\\tclass_attr_{n}\\t\" + format((time.monotonic()-t0)*1000.0, \".3f\"))",
        "t0 = time.monotonic()", "s = 0", "i = 0",
        "while i < 400000:",
        f"    s = s + obj.m0() + obj.m{n-1}()",
        "    i = i + 1",
        f"io.print(\"RESULT\\tclass_method_{n}\\t\" + format((time.monotonic()-t0)*1000.0, \".3f\"))",
        "discard s",
    ]
    return "\n".join(L) + "\n"


def gen_py(n):
    L = ["import time", "class Big:", "    def __init__(self):"]
    for i in range(n):
        L.append(f"        self.attr{i} = {i}")
    for i in range(n):
        L.append(f"    def m{i}(self): return {i}")
    L += [
        "obj = Big()",
        "t0 = time.perf_counter()", "s = 0", "i = 0",
        "while i < 400000:",
        f"    s = s + obj.attr0 + obj.attr{n-1}",
        "    i = i + 1",
        f"print('RESULT\\tclass_attr_{n}\\t%.3f' % ((time.perf_counter()-t0)*1000.0))",
        "t0 = time.perf_counter()", "s = 0", "i = 0",
        "while i < 400000:",
        f"    s = s + obj.m0() + obj.m{n-1}()",
        "    i = i + 1",
        f"print('RESULT\\tclass_method_{n}\\t%.3f' % ((time.perf_counter()-t0)*1000.0))",
    ]
    return "\n".join(L) + "\n"


def gen_lua(n):
    L = ["local Big = {}", "Big.__index = Big", "function Big.new()",
         "  local self = setmetatable({}, Big)"]
    for i in range(n):
        L.append(f"  self.attr{i} = {i}")
    L.append("  return self")
    L.append("end")
    for i in range(n):
        L.append(f"function Big:m{i}() return {i} end")
    L += [
        "local obj = Big.new()",
        "local t0 = os.clock()", "local s = 0", "local i = 0",
        "while i < 400000 do",
        f"  s = s + obj.attr0 + obj.attr{n-1}",
        "  i = i + 1",
        "end",
        f"io.write(string.format('RESULT\\tclass_attr_{n}\\t%.3f\\n', (os.clock()-t0)*1000.0))",
        "t0 = os.clock()", "s = 0", "i = 0",
        "while i < 400000 do",
        f"  s = s + obj:m0() + obj:m{n-1}()",
        "  i = i + 1",
        "end",
        f"io.write(string.format('RESULT\\tclass_method_{n}\\t%.3f\\n', (os.clock()-t0)*1000.0))",
    ]
    return "\n".join(L) + "\n"


def run_cmd(cmd, timeout=600):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if p.returncode != 0:
        return None, p.stderr
    return p.stdout, None


def time_fixed(cmd, runs):
    best = None
    for _ in range(runs):
        out, err = run_cmd(cmd)
        if out is None:
            return None
        try:
            ms = float(out.strip().splitlines()[-1])
        except (ValueError, IndexError):
            return None
        best = ms if best is None else min(best, ms)
    return best


def run_scaling(cmd_of, runtimes, runs):
    """cmd_of(runtime_key) -> command; returns {result_name: {rt: ms}}."""
    results = {}
    for key, cmd in runtimes:
        agg = {}
        for _ in range(runs):
            out, err = run_cmd(cmd_of(key, cmd))
            if out is None:
                break
            for line in out.splitlines():
                p = line.split("\t")
                if len(p) == 3 and p[0] == "RESULT":
                    nm, ms = p[1], float(p[2])
                    agg[nm] = ms if nm not in agg else min(agg[nm], ms)
        for nm, ms in agg.items():
            results.setdefault(nm, {})[key] = ms
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--python", default="python3")
    ap.add_argument("--lua", default="lua")
    ap.add_argument("--runs", type=int, default=3)
    args = ap.parse_args()

    lua = shutil.which(args.lua) or args.lua
    py = shutil.which(args.python) or args.python

    def ver(ki):
        try:
            return subprocess.run([ki, "--version"], capture_output=True, text=True).stdout.strip()
        except Exception:
            return ki

    # runtime key -> how to invoke a fixed workload `wl`
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
    print(f"runs/cell  : {args.runs} (best kept)   started {time.strftime('%H:%M:%S')}")
    print("\nAll times in milliseconds (lower is better). 'x' column = 1.17.1 / 1.18.0 speedup.\n")

    hdr = f"{'workload':<18}" + "".join(f"{c:>11}" for c in cols) + f"{'1.17->1.18':>12}"

    speedups = []
    for group, wls in FIXED:
        print(f"--- {group} ---")
        print(hdr)
        for wl in wls:
            cells = {}
            for key, mk in rts:
                cells[key] = time_fixed(mk(wl), args.runs)
            row = f"{wl:<18}"
            for c in cols:
                v = cells.get(c)
                row += f"{('%.2f' % v) if v is not None else '-':>11}"
            o, n = cells.get("1.17.1"), cells.get("1.18.0")
            if o and n and n > 0:
                sp = o / n
                speedups.append((wl, sp))
                row += f"{('%.2fx' % sp):>12}"
            else:
                row += f"{'-':>12}"
            print(row)
        print()

    # ---- class attribute/method scaling ----
    print("--- class scaling (N attributes + N methods; time a fixed access) ---")
    print(hdr)
    tmp = tempfile.mkdtemp(prefix="kirito_scale_")
    files = {}
    for n in SCALE_NS:
        files[("ki", n)] = os.path.join(tmp, f"s_{n}.ki"); open(files[("ki", n)], "w").write(gen_ki(n))
        files[("py", n)] = os.path.join(tmp, f"s_{n}.py"); open(files[("py", n)], "w").write(gen_py(n))
        files[("lua", n)] = os.path.join(tmp, f"s_{n}.lua"); open(files[("lua", n)], "w").write(gen_lua(n))

    scale_results = {}  # name -> {col: ms}
    for n in SCALE_NS:
        runtimes = [
            ("1.17.1", [args.old, files[("ki", n)]]),
            ("1.18.0", [args.new, files[("ki", n)]]),
            ("Python3", [py, files[("py", n)]]),
            ("Lua5.1", [lua, files[("lua", n)]]),
        ]
        res = run_scaling(lambda key, cmd: cmd, runtimes, args.runs)
        for nm, d in res.items():
            scale_results[nm] = d
    for nm in sorted(scale_results, key=lambda s: (s.split("_")[1], int(s.split("_")[-1]))):
        d = scale_results[nm]
        row = f"{nm:<18}"
        for c in cols:
            v = d.get(c)
            row += f"{('%.2f' % v) if v is not None else '-':>11}"
        o, n2 = d.get("1.17.1"), d.get("1.18.0")
        if o and n2 and n2 > 0:
            sp = o / n2
            speedups.append((nm, sp))
            row += f"{('%.2fx' % sp):>12}"
        else:
            row += f"{'-':>12}"
        print(row)
    shutil.rmtree(tmp, ignore_errors=True)

    if speedups:
        import math
        gm = math.exp(sum(math.log(s) for _, s in speedups if s > 0) / len(speedups))
        print(f"\nGeometric-mean 1.17.1 -> 1.18.0 speedup across {len(speedups)} workloads: {gm:.2f}x")
        best = max(speedups, key=lambda x: x[1]); worst = min(speedups, key=lambda x: x[1])
        print(f"  best:  {best[0]} {best[1]:.2f}x     worst: {worst[0]} {worst[1]:.2f}x")


if __name__ == "__main__":
    main()
