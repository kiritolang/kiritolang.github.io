// A small benchmark/correctness harness: runs representative workloads, checks results against
// known baselines, and reports timing. Passes as long as results are correct.
#include <chrono>
#include <cstdio>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

template <typename F>
static double timeMs(F&& f) {
    auto t0 = std::chrono::steady_clock::now();
    f();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main() {
    KiritoVM vm;

    // Recursive fibonacci (function-call + arithmetic heavy).
    std::string fibResult;
    double tFib = timeMs([&] {
        fibResult = vm.stringify(vm.runSource(
            "var fib = Function(n):\n"
            "    if n < 2:\n"
            "        return n\n"
            "    return fib(n - 1) + fib(n - 2)\n"
            "fib(27)\n"));
    });
    CHECK(fibResult == "196418");

    // A tight numeric loop (1,000,000 iterations).
    std::string sumResult;
    double tLoop = timeMs([&] {
        sumResult = vm.stringify(vm.runSource(
            "var s = 0\n"
            "var i = 0\n"
            "while i < 1000000:\n"
            "    s = s + i\n"
            "    i = i + 1\n"
            "s\n"));
    });
    CHECK(sumResult == "499999500000");

    // A tight numeric loop INSIDE a function (1,000,000 iterations) — exercises slot-addressed locals
    // (s/i/a/b are non-captured function locals) plus the numeric fast path. The module-scope loop
    // above, by contrast, is name-based; this is the workload slot-addressing targets.
    std::string fnLoopResult;
    double tFnLoop = timeMs([&] {
        fnLoopResult = vm.stringify(vm.runSource(
            "var run = Function(n):\n"
            "    var s = 0\n"
            "    var i = 0\n"
            "    while i < n:\n"
            "        var a = i * 2\n"
            "        var b = a + 3\n"
            "        s = s + a + b\n"
            "        i = i + 1\n"
            "    return s\n"
            "run(1000000)\n"));
    });
    CHECK(fnLoopResult == "2000001000000");

    // List building and summation.
    std::string listResult;
    double tList = timeMs([&] {
        listResult = vm.stringify(vm.runSource(
            "var xs = []\n"
            "var i = 0\n"
            "while i < 100000:\n"
            "    xs.append(i)\n"
            "    i = i + 1\n"
            "var total = 0\n"
            "for x in xs:\n"
            "    total = total + x\n"
            "total\n"));
    });
    CHECK(listResult == "4999950000");

    // String building.
    std::string strResult;
    double tStr = timeMs([&] {
        strResult = vm.stringify(vm.runSource(
            "var parts = []\n"
            "var i = 0\n"
            "while i < 20000:\n"
            "    parts.append(\"x\")\n"
            "    i = i + 1\n"
            "len(\"\".join(parts))\n"));
    });
    CHECK(strResult == "20000");

    std::printf("bench: fib(27) %.0f ms | loop 1e6 %.0f ms | fnloop 1e6 %.0f ms | list 1e5 %.0f ms | "
                "strjoin 2e4 %.0f ms | live=%zu\n",
                tFib, tLoop, tFnLoop, tList, tStr, vm.liveCount());
    return RUN_TESTS();
}
