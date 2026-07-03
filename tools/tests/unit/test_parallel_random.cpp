// Randomized parallel map-reduce: split range(n) across k workers (each returns a partial sum) and
// check the total equals the serial baseline, for many seeded (n, k) shapes.

#include <random>
#include <string>

#include "../parallel_util.hpp"

using namespace kitest;

int main() {
    std::mt19937 rng(0xC0FFEE);
    for (int trial = 0; trial < 8; ++trial) {
        int n = 40 + static_cast<int>(rng() % 260);
        int k = 1 + static_cast<int>(rng() % 6);
        std::string src =
            "var parallel = import(\"parallel\")\n"
            "var partial = Function(lo, hi):\n"
            "    var s = 0\n"
            "    var i = lo\n"
            "    while i < hi:\n"
            "        s = s + i\n"
            "        i = i + 1\n"
            "    return s\n"
            "if argmain:\n"
            "    var n = " + std::to_string(n) + "\n"
            "    var k = " + std::to_string(k) + "\n"
            "    var chunk = (n + k - 1) // k\n"
            "    var tasks = []\n"
            "    var lo = 0\n"
            "    while lo < n:\n"
            "        var hi = lo + chunk\n"
            "        if hi > n:\n"
            "            hi = n\n"
            "        tasks.append(parallel.spawn(partial, lo, hi))\n"
            "        lo = lo + chunk\n"
            "    var total = 0\n"
            "    for t in tasks:\n"
            "        total = total + t.join()\n"
            "    assert total == n * (n - 1) // 2\n";
        expectOk(("mapreduce n=" + std::to_string(n) + " k=" + std::to_string(k)).c_str(), src);
    }
    return RUN_TESTS();
}
