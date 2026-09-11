#!/usr/bin/env python3
"""Compare two `ki` binaries on the churn benchmark across GC-churn modes.

Runs tests/bench/churn_bench.ki under several KIRITO_GC_THRESHOLD settings ("churn modes") against an
OLD and a NEW interpreter. Each workload is measured over >= 10 runs and reported as mean +/- sample
stddev (milliseconds); the speedup column is old_mean / new_mean (>1.0 = the new build is faster).

Usage:
    python3 tests/bench/churn_compare.py --old dist/ki-linux-x64 --new build-bin/ki-release [--runs 10]
"""
import argparse
import math
import os
import statistics
import subprocess
import time

# (label, KIRITO_GC_THRESHOLD or None for the default adaptive cadence)
CHURN_MODES = [
    ("high churn (GC=2000)", "2000"),
    ("default churn", None),
    ("low churn (GC=1e8)", "100000000"),
]
COLW = 18


def r(v):
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


def run_once(ki, script, threshold):
    env = dict(os.environ)
    if threshold is None:
        env.pop("KIRITO_GC_THRESHOLD", None)
    else:
        env["KIRITO_GC_THRESHOLD"] = threshold
    proc = subprocess.run([ki, script], capture_output=True, text=True, env=env, timeout=900)
    if proc.returncode != 0:
        raise SystemExit(f"FAILED: {ki} (threshold={threshold}) exit {proc.returncode}\n{proc.stderr}")
    out, order = {}, []
    for line in proc.stdout.splitlines():
        p = line.split("\t")
        if len(p) == 3 and p[0] == "RESULT":
            if p[1] not in out:
                order.append(p[1])
            out[p[1]] = float(p[2])
    return order, out


def samples(ki, script, threshold, runs):
    order, agg = None, {}
    for _ in range(runs):
        order, res = run_once(ki, script, threshold)
        for k, v in res.items():
            agg.setdefault(k, []).append(v)
    return order, agg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True, help="baseline ki (e.g. dist/ki-linux-x64, 1.17.1)")
    ap.add_argument("--new", required=True, help="new ki (e.g. build-bin/ki-release, 1.18.0)")
    ap.add_argument("--script", default="tests/bench/churn_bench.ki")
    ap.add_argument("--runs", type=int, default=10)
    args = ap.parse_args()
    runs = max(10, args.runs)

    for p in (args.old, args.new, args.script):
        if not os.path.exists(p):
            raise SystemExit(f"not found: {p}")

    def ver(ki):
        try:
            return subprocess.run([ki, "--version"], capture_output=True, text=True).stdout.strip()
        except Exception:
            return ki

    print(f"OLD: {args.old}   [{ver(args.old)}]")
    print(f"NEW: {args.new}   [{ver(args.new)}]")
    print(f"runs/cell: {runs} (mean±stddev)   started {time.strftime('%Y-%m-%d %H:%M:%S')}")
    print("\nCells are mean±stddev in milliseconds (lower is better). speedup = 1.17.1/1.18.0.\n")

    perf = []
    for label, thr in CHURN_MODES:
        order_o, old = samples(args.old, args.script, thr, runs)
        order_n, new = samples(args.new, args.script, thr, runs)
        order = order_o or order_n
        print(f"=== churn mode: {label} ===")
        print(f"{'workload':<16}{'1.17.1':>{COLW}}{'1.18.0':>{COLW}}{'speedup':>10}")
        print("-" * (16 + 2 * COLW + 10))
        for name in order:
            ov, nv = old.get(name, []), new.get(name, [])
            row = f"{name:<16}{cell(ov):>{COLW}}{cell(nv):>{COLW}}"
            if ov and nv:
                sp = statistics.fmean(ov) / statistics.fmean(nv)
                if sp > 0:
                    perf.append(sp)
                row += f"{('%.2fx' % sp):>10}"
            else:
                row += f"{'-':>10}"
            print(row)
        print()

    if perf:
        gm = math.exp(sum(math.log(s) for s in perf) / len(perf))
        print(f"Geometric-mean speedup (all modes): {gm:.2f}x")
    print("Note: `isprime` is deterministic trial division in both versions (near-parity). The")
    print("      deterministic AKS test is a separate function, `isprimeaks` (far slower for large n).")


if __name__ == "__main__":
    main()
