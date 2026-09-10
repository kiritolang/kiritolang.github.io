#!/usr/bin/env python3
# Cross-language benchmark — Python side. `python3 bench.py <workload>` runs one workload and prints
# its wall time in milliseconds. Mirrors bench.ki / bench.lua exactly (same algorithm + counts).
import sys
import time


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def ack(m, n):
    if m == 0:
        return n + 1
    if n == 0:
        return ack(m - 1, 1)
    return ack(m - 1, ack(m, n - 1))


def gcd(a, b):
    while b != 0:
        a, b = b, a % b
    return a


class Vec:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def norm2(self):
        return self.x * self.x + self.y * self.y


class Shape:
    def area(self):
        return 0


class Sq(Shape):
    def __init__(self, s):
        self.s = s

    def area(self):
        return self.s * self.s


class Ci(Shape):
    def __init__(self, r):
        self.r = r

    def area(self):
        return 3 * self.r * self.r


def qsort(a):
    if len(a) < 2:
        return a
    pivot = a[len(a) // 2]
    lo = [x for x in a if x < pivot]
    eq = [x for x in a if x == pivot]
    hi = [x for x in a if x > pivot]
    return qsort(lo) + eq + qsort(hi)


def run(wl):
    if wl == "fib":
        return fib(30)
    if wl == "ackermann":
        return ack(3, 6)
    if wl == "sum_loop":
        s = 0
        for _ in range(2000):
            i = 0
            while i < 1000:
                s = s + i
                i = i + 1
        return s
    if wl == "float_loop":
        s = 0.0
        for _ in range(2000):
            i = 0
            while i < 1000:
                s = s + float(i) * 0.5
                i = i + 1
        return int(s % 1000.0)
    if wl == "nested_loop":
        s = 0
        i = 0
        while i < 700:
            j = 0
            while j < 700:
                s = s + i * j
                j = j + 1
            i = i + 1
        return s
    if wl == "sieve":
        s = 0
        for _ in range(300):
            sieve = []
            k = 0
            while k < 5000:
                sieve.append(True)
                k = k + 1
            p = 2
            while p * p < 5000:
                if sieve[p]:
                    m = p * p
                    while m < 5000:
                        sieve[m] = False
                        m = m + p
                p = p + 1
            c = 0
            q = 2
            while q < 5000:
                if sieve[q]:
                    c = c + 1
                q = q + 1
            s = c
        return s
    if wl == "collatz":
        total = 0
        n = 1
        while n < 30000:
            x = n
            steps = 0
            while x != 1:
                if x % 2 == 0:
                    x = x // 2
                else:
                    x = 3 * x + 1
                steps = steps + 1
            total = total + steps
            n = n + 1
        return total
    if wl == "gcd_loop":
        s = 0
        i = 1
        while i < 200000:
            s = s + gcd(i, 12345)
            i = i + 1
        return s
    if wl == "list_build":
        n = 0
        for _ in range(2000):
            a = []
            i = 0
            while i < 1000:
                a.append(i)
                i = i + 1
            n = len(a)
        return n
    if wl == "list_sum":
        a = []
        i = 0
        while i < 10000:
            a.append(i)
            i = i + 1
        s = 0
        for _ in range(2000):
            for x in a:
                s = s + x
        return s
    if wl == "list_sort":
        s = 0
        for _ in range(2000):
            a = []
            i = 0
            seed = 12345
            while i < 1000:
                seed = (seed * 1103515245 + 12345) % 2147483648
                a.append(seed % 100000)
                i = i + 1
            a.sort()
            s = a[0]
        return s
    if wl == "quicksort":
        s = 0
        for _ in range(300):
            a = []
            i = 0
            seed = 999
            while i < 1000:
                seed = (seed * 1103515245 + 12345) % 2147483648
                a.append(seed % 100000)
                i = i + 1
            sortd = qsort(a)
            s = sortd[0]
        return s
    if wl == "dict_build":
        n = 0
        for _ in range(1000):
            d = {}
            i = 0
            while i < 2000:
                d[i] = i * 2
                i = i + 1
            n = len(d)
        return n
    if wl == "dict_lookup_int":
        d = {}
        i = 0
        while i < 5000:
            d[i] = i
            i = i + 1
        s = 0
        for _ in range(300):
            k = 0
            while k < 5000:
                s = s + d[k]
                k = k + 1
        return s
    if wl == "dict_lookup_str":
        d = {}
        i = 0
        while i < 5000:
            d["key" + str(i)] = i
            i = i + 1
        s = 0
        for _ in range(300):
            k = 0
            while k < 5000:
                s = s + d["key" + str(k)]
                k = k + 1
        return s
    if wl == "set_ops":
        s = 0
        for _ in range(2000):
            a = set()
            i = 0
            while i < 500:
                a.add(i)
                i = i + 1
            hits = 0
            k = 0
            while k < 1000:
                if k in a:
                    hits = hits + 1
                k = k + 1
            s = hits
        return s
    if wl == "str_concat":
        n = 0
        for _ in range(3000):
            s = ""
            i = 0
            while i < 300:
                s = s + "x"
                i = i + 1
            n = len(s)
        return n
    if wl == "str_split_join":
        text = ""
        i = 0
        while i < 2000:
            text = text + "word" + str(i) + " "
            i = i + 1
        n = 0
        for _ in range(500):
            parts = text.split(" ")
            joined = "-".join(parts)
            n = len(joined)
        return n
    if wl == "str_search":
        text = ""
        i = 0
        while i < 3000:
            text = text + "abcdefg" + str(i % 10)
            i = i + 1
        c = 0
        for _ in range(1000):
            c = text.count("5")
        return c
    if wl == "method_call":
        v = Vec(3, 4)
        s = 0
        i = 0
        while i < 500000:
            s = s + v.norm2()
            i = i + 1
        return s
    if wl == "attr_rw":
        v = Vec(0, 0)
        i = 0
        while i < 500000:
            v.x = v.x + 1
            v.y = v.x + v.y
            i = i + 1
        return v.y
    if wl == "object_create":
        s = 0
        i = 0
        while i < 300000:
            v = Vec(i, i + 1)
            s = s + v.x
            i = i + 1
        return s
    if wl == "poly_dispatch":
        shapes = []
        i = 0
        while i < 1000:
            shapes.append(Sq(i) if i % 2 == 0 else Ci(i))
            i = i + 1
        s = 0
        for _ in range(300):
            for sh in shapes:
                s = s + sh.area()
        return s
    if wl == "map_filter":
        a = list(range(2000))
        n = 0
        for _ in range(100):
            n = len(list(filter(lambda x: x % 2 == 0, map(lambda x: x * x, a))))
        return n
    if wl == "matmul_manual":
        N = 40
        A = []
        B = []
        i = 0
        while i < N:
            ra = []
            rb = []
            j = 0
            while j < N:
                ra.append(i + j)
                rb.append(i - j)
                j = j + 1
            A.append(ra)
            B.append(rb)
            i = i + 1
        s = 0
        for _ in range(30):
            C = []
            r = 0
            while r < N:
                row = []
                c = 0
                while c < N:
                    acc = 0
                    k = 0
                    while k < N:
                        acc = acc + A[r][k] * B[k][c]
                        k = k + 1
                    row.append(acc)
                    c = c + 1
                C.append(row)
                r = r + 1
            s = C[0][0]
        return s
    return 0


def main():
    sys.setrecursionlimit(1000000)
    wl = sys.argv[1]
    t0 = time.perf_counter()
    _ = run(wl)
    print("%.3f" % ((time.perf_counter() - t0) * 1000.0))


if __name__ == "__main__":
    main()
