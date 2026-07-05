// test_collections_deep.cpp — adversarial/edge coverage for List/Set/Dict, filling audited gaps:
// Set.apply error propagation, List.index negative-end window, Set set-algebra missing-arg throws,
// NaN in List value-search, negative-step-with-bounds + float slice bound, Dict pop/setdefault
// unhashable-key throws, Dict multi-key subscript throw, and Float key-identity edges (±0.0, ±inf).
#include <string>
#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; }
    catch (...) { return true; }
}

int main() {
    KiritoVM vm;

    // ---- Set.apply propagates an exception from fn (parity with List/Dict apply) ----
    CHECK(throws(vm, "{1, 2, 3}.apply(Function(x): throw \"boom\")"));
    CHECK(run(vm, "len({1, 2, 3}.apply(Function(x): return x * 2))") == "3");

    // ---- List.index with an explicit (start, end) window incl. a negative end ----
    CHECK(run(vm, "[1, 2, 3, 2].index(2)") == "1");
    CHECK(run(vm, "[1, 2, 3, 2].index(2, 2)") == "3");            // start past the first match
    CHECK(run(vm, "[1, 2, 3, 2].index(2, 0, -1)") == "1");        // negative end -> len-1, first 2 still at 1
    CHECK(throws(vm, "[1, 2, 3, 2].index(2, 2, -1)"));            // window [2, 3) holds only 3

    // ---- Set algebra with a missing argument throws ("expects an iterable") ----
    CHECK(throws(vm, "{1, 2}.union()"));
    CHECK(throws(vm, "{1, 2}.intersection()"));
    CHECK(throws(vm, "{1, 2}.difference()"));
    CHECK(throws(vm, "{1, 2}.symmetricdifference()"));
    CHECK(throws(vm, "{1, 2}.issubset()"));
    CHECK(throws(vm, "{1, 2}.issuperset()"));
    CHECK(throws(vm, "{1, 2}.isdisjoint()"));

    // ---- NaN in a List value-search: exact ==, so it is never found ----
    CHECK(run(vm, "var math = import(\"math\")\n[math.nan].count(math.nan)") == "0");
    CHECK(throws(vm, "var math = import(\"math\")\n[math.nan].index(math.nan)"));
    CHECK(throws(vm, "var math = import(\"math\")\n[math.nan].remove(math.nan)"));

    // ---- List slice: negative step WITH bounds, and a float bound throws ----
    CHECK(run(vm, "[0, 1, 2, 3, 4][4:1:-1]") == "[4, 3, 2]");
    CHECK(run(vm, "[0, 1, 2, 3, 4][::-2]") == "[4, 2, 0]");
    CHECK(throws(vm, "[1, 2, 3][1.5:]"));
    CHECK(throws(vm, "[1, 2, 3][::0]"));                          // zero step

    // ---- Dict pop / setdefault with an unhashable key throw (same guard as get) ----
    CHECK(throws(vm, "{1: 2}.pop([])"));
    CHECK(throws(vm, "{1: 2}.setdefault([], 0)"));
    CHECK(throws(vm, "{1: 2}.get([])"));

    // ---- Dict multi-key subscript is rejected (a Dict takes exactly one index) ----
    CHECK(throws(vm, "{1: 2}[0, 1]"));
    CHECK(throws(vm, "var d = {1: 2}\nd[0, 1] = 5"));

    // ---- Float key identity: +0.0 and -0.0 collapse; +inf and -inf are distinct ----
    CHECK(run(vm, "var d = {}\nd[0.0] = 1\nd[-0.0] = 2\nlen(d)") == "1");
    CHECK(run(vm, "var d = {}\nd[0.0] = 1\nd[-0.0] = 2\nd[0.0]") == "2");
    CHECK(run(vm, "var math = import(\"math\")\nlen({math.inf, math.inf})") == "1");
    CHECK(run(vm, "var math = import(\"math\")\nlen({math.inf, -math.inf})") == "2");
    // NaN keys are each distinct (exact identity, never equal — write-only keys, a documented invariant)
    CHECK(run(vm, "var math = import(\"math\")\nlen({math.nan, math.nan})") == "2");

    return RUN_TESTS();
}
