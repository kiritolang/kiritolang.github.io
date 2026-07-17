// C++ baseline for the cross-language benchmark (Kirito vs C++ vs Python).
//   usage: compare_bench <workload> <N> <reps>
// Runs the named workload `reps` times, timing each run with steady_clock, and prints
//   <mean_ns> <stddev_ns>
// on stdout. A deterministic 31-bit LCG (identical in all three languages) generates inputs so the
// three implementations do the same work. The workloads split into:
//   pessimistic (interpreter-bound loops): sum_loop, fib, sieve
//   optimistic  (delegate to library/builtins): sort, dict_ops, string_ops
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

// Shared LCG: state = (state*1103515245 + 12345) & 0x7fffffff. Stays in 31 bits, so the multiply
// never overflows int64 — letting Kirito (int64) and Python reproduce the exact same sequence.
struct Lcg {
    int64_t s;
    explicit Lcg(int64_t seed) : s(seed) {}
    int64_t next() { s = (s * 1103515245 + 12345) & 0x7fffffff; return s; }
};

static int64_t fib(int64_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

// Each workload returns a checksum-ish long (printed to stderr) to defeat dead-code elimination.
static long run_sum_loop(long N) {
    long s = 0;
    for (long i = 0; i < N; ++i) s += i * 2 - i;
    return s;
}
static long run_fib(long N) { return static_cast<long>(fib(N)); }
static long run_sieve(long N) {
    std::vector<char> sieve(N + 1, 1);
    long count = 0;
    for (long p = 2; p <= N; ++p) {
        if (sieve[p]) {
            ++count;
            for (long m = p * p; m <= N; m += p) sieve[m] = 0;
        }
    }
    return count;
}
static long run_sort(const std::vector<int64_t>& base) {
    std::vector<int64_t> a = base;        // fresh copy each rep (builtin copy + sort)
    std::sort(a.begin(), a.end());
    return static_cast<long>(a.front() + a.back() + a[a.size() / 2]);
}
static long run_dict_ops(const std::vector<int64_t>& keys) {
    std::unordered_map<int64_t, int64_t> d;
    d.reserve(keys.size() * 2);
    for (int64_t k : keys) d[k] = k * k;
    long sum = 0;
    for (int64_t k : keys) sum += d[k];
    return sum;
}
static long run_string_ops(const std::string& text) {
    // split on spaces, then join with '-' (library-heavy string work)
    std::vector<std::string> words;
    size_t start = 0;
    while (start <= text.size()) {
        size_t sp = text.find(' ', start);
        if (sp == std::string::npos) { words.push_back(text.substr(start)); break; }
        words.push_back(text.substr(start, sp - start));
        start = sp + 1;
    }
    std::string joined;
    for (size_t i = 0; i < words.size(); ++i) { if (i) joined += '-'; joined += words[i]; }
    return static_cast<long>(joined.size() + words.size());
}

int main(int argc, char** argv) {
    if (argc != 4) { std::fprintf(stderr, "usage: %s <workload> <N> <reps>\n", argv[0]); return 2; }
    std::string wl = argv[1];
    long N = std::stol(argv[2]);
    long reps = std::stol(argv[3]);

    // Pre-build inputs for the optimistic workloads (excluded from per-rep timing).
    std::vector<int64_t> base, keys;
    std::string text;
    if (wl == "sort")      { Lcg g(12345); for (long i = 0; i < N; ++i) base.push_back(g.next() % 1000000); }
    if (wl == "dict_ops")  { Lcg g(777);   for (long i = 0; i < N; ++i) keys.push_back(g.next()); }
    if (wl == "string_ops"){ Lcg g(99);    for (long i = 0; i < N; ++i) { if (i) text += ' '; text += "w" + std::to_string(g.next() % 10000); } }

    auto once = [&]() -> long {
        if (wl == "sum_loop")   return run_sum_loop(N);
        if (wl == "fib")        return run_fib(N);
        if (wl == "sieve")      return run_sieve(N);
        if (wl == "sort")       return run_sort(base);
        if (wl == "dict_ops")   return run_dict_ops(keys);
        if (wl == "string_ops") return run_string_ops(text);
        std::fprintf(stderr, "unknown workload '%s'\n", wl.c_str());
        std::exit(2);
    };

    long sink = once();  // warmup (also primes caches)
    std::vector<double> samples;
    samples.reserve(reps);
    for (long r = 0; r < reps; ++r) {
        auto t0 = std::chrono::steady_clock::now();
        sink ^= once();
        auto t1 = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }

    double mean = 0;
    for (double v : samples) mean += v;
    mean /= samples.size();
    double var = 0;
    for (double v : samples) var += (v - mean) * (v - mean);
    var /= samples.size();
    std::printf("%.1f %.1f\n", mean, std::sqrt(var));
    std::fprintf(stderr, "checksum=%ld\n", sink);  // keep `sink` live
    return 0;
}
