#!/usr/bin/env python3
# Python baseline for the cross-language benchmark. Same workloads, same LCG, same protocol as
# compare_bench.cpp:  compare_bench.py <workload> <N> <reps>  ->  prints "<mean_ns> <stddev_ns>".
import sys
import time
import math


class Lcg:
    def __init__(self, seed):
        self.s = seed

    def next(self):
        self.s = (self.s * 1103515245 + 12345) & 0x7FFFFFFF
        return self.s


def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)


def run_sum_loop(N):
    s = 0
    for i in range(N):
        s += i * 2 - i
    return s


def run_fib(N):
    return fib(N)


def run_sieve(N):
    sieve = [True] * (N + 1)
    count = 0
    p = 2
    while p <= N:
        if sieve[p]:
            count += 1
            m = p * p
            while m <= N:
                sieve[m] = False
                m += p
        p += 1
    return count


def run_sort(base):
    a = list(base)
    a.sort()
    return a[0] + a[-1] + a[len(a) // 2]


def run_dict_ops(keys):
    d = {}
    for k in keys:
        d[k] = k * k
    return sum(d[k] for k in keys)


def run_string_ops(text):
    words = text.split(" ")
    joined = "-".join(words)
    return len(joined) + len(words)


def main():
    wl, N, reps = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

    base = keys = text = None
    if wl == "sort":
        g = Lcg(12345); base = [g.next() % 1000000 for _ in range(N)]
    elif wl == "dict_ops":
        g = Lcg(777); keys = [g.next() for _ in range(N)]
    elif wl == "string_ops":
        g = Lcg(99); text = " ".join("w" + str(g.next() % 10000) for _ in range(N))

    def once():
        if wl == "sum_loop":   return run_sum_loop(N)
        if wl == "fib":        return run_fib(N)
        if wl == "sieve":      return run_sieve(N)
        if wl == "sort":       return run_sort(base)
        if wl == "dict_ops":   return run_dict_ops(keys)
        if wl == "string_ops": return run_string_ops(text)
        sys.stderr.write("unknown workload '%s'\n" % wl)
        sys.exit(2)

    sink = once()  # warmup
    samples = []
    for _ in range(reps):
        t0 = time.perf_counter_ns()
        sink ^= once()
        t1 = time.perf_counter_ns()
        samples.append(float(t1 - t0))

    mean = sum(samples) / len(samples)
    var = sum((v - mean) ** 2 for v in samples) / len(samples)
    print("%.1f %.1f" % (mean, math.sqrt(var)))
    sys.stderr.write("checksum=%d\n" % sink)


if __name__ == "__main__":
    main()
