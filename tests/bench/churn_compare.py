#!/usr/bin/env python3
"""Compare two `ki` binaries on the churn benchmark across GC-churn modes.

Runs tests/bench/churn_bench.ki under several KIRITO_GC_THRESHOLD settings ("churn modes") against an
OLD and a NEW interpreter, takes the best (min) of a few runs per workload to cut noise, and prints a
per-mode table of old vs new milliseconds and the speedup (old/new; >1.0 = the new build is faster).

Usage:
    python3 tests/bench/churn_compare.py --old dist/ki-linux-x64 --new build-bin/ki-release
    python3 tests/bench/churn_compare.py --old <1.17.1> --new <1.18.0> --runs 5
"""
import argparse
import os
import subprocess
import sys
import time

# (label, KIRITO_GC_THRESHOLD or None for the default adaptive cadence)
CHURN_MODES = [
    ("high churn (GC=2000)", "2000"),
    ("default churn", None),
    ("low churn (GC=1e8)", "100000000"),
]


def run_once(ki, script, threshold):
    env = dict(os.environ)
    if threshold is None:
        env.pop("KIRITO_GC_THRESHOLD", None)
    else:
        env["KIRITO_GC_THRESHOLD"] = threshold
    proc = subprocess.run([ki, script], capture_output=True, text=True, env=env, timeout=600)
    if proc.returncode != 0:
        sys.exit(f"FAILED: {ki} (threshold={threshold}) exit {proc.returncode}\n{proc.stderr}")
    out = {}
    order = []
    for line in proc.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) == 3 and parts[0] == "RESULT":
            name, ms = parts[1], float(parts[2])
            if name not in out:
                order.append(name)
            out[name] = ms
    return order, out


def best_of(ki, script, threshold, runs):
    order, best = None, {}
    for _ in range(runs):
        order, res = run_once(ki, script, threshold)
        for k, v in res.items():
            best[k] = v if k not in best else min(best[k], v)
    return order, best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True, help="baseline ki binary (e.g. dist/ki-linux-x64, 1.17.1)")
    ap.add_argument("--new", required=True, help="new ki binary (e.g. build-bin/ki-release, 1.18.0)")
    ap.add_argument("--script", default="tests/bench/churn_bench.ki")
    ap.add_argument("--runs", type=int, default=3, help="runs per (binary, mode); best/min is kept")
    args = ap.parse_args()

    for p in (args.old, args.new, args.script):
        if not os.path.exists(p):
            sys.exit(f"not found: {p}")

    def ver(ki):
        try:
            return subprocess.run([ki, "--version"], capture_output=True, text=True).stdout.strip()
        except Exception:
            return ki

    print(f"OLD: {args.old}   [{ver(args.old)}]")
    print(f"NEW: {args.new}   [{ver(args.new)}]")
    print(f"runs/mode: {args.runs} (best kept)   started: {time.strftime('%Y-%m-%d %H:%M:%S')}")

    overall = []
    for label, thr in CHURN_MODES:
        order_o, old = best_of(args.old, args.script, thr, args.runs)
        order_n, new = best_of(args.new, args.script, thr, args.runs)
        order = order_o or order_n
        print(f"\n=== churn mode: {label} ===")
        print(f"{'workload':<16}{'1.17.1 ms':>12}{'1.18.0 ms':>12}{'speedup':>10}")
        print("-" * 50)
        for name in order:
            o = old.get(name)
            n = new.get(name)
            if o is None or n is None:
                continue
            sp = (o / n) if n > 0 else float("inf")
            overall.append((label, name, o, n, sp))
            print(f"{name:<16}{o:>12.3f}{n:>12.3f}{sp:>9.2f}x")

    # geometric-mean speedup (excluding isprime, whose AKS is a deliberate correctness tradeoff)
    import math
    perf = [sp for (_, name, _, _, sp) in overall if name != "isprime_small" and sp > 0]
    if perf:
        gm = math.exp(sum(math.log(s) for s in perf) / len(perf))
        print(f"\nGeometric-mean speedup (all modes, excl. isprime AKS tradeoff): {gm:.2f}x")
    isp = [(m, o, n) for (m, name, o, n, _) in overall if name == "isprime_small"]
    if isp:
        print("Note: isprime_small is SLOWER on 1.18.0 by design — deterministic AKS replaces trial")
        print("      division (AKS is exact/polynomial but far slower for small n; use isprobableprime).")


if __name__ == "__main__":
    main()
