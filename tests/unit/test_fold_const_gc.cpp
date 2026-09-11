// Regression: constant folding (A2, v1.18.0) must keep folded NON-INTERNED constants rooted through
// the rest of compilation. `tryEmitFolded` adds the fold to the Proto const pool via addConst()'s
// pushTemp, but it did so INSIDE a local RootScope whose destructor truncates the SHARED tempRoots_
// stack (popTempTo) — popping the fold's root the instant tryEmitFolded returned. A non-interned
// folded const (large Int / Float / String) was then unrooted, and a later compile-time allocation
// under aggressive GC swept it; the runtime `LoadConst` then threw "dangling handle (stale
// generation)". Interned folds (small int / Bool / None) never allocate a sweepable object, which
// masked the bug at default GC. Each block is driven at setGcThreshold(1) and has a trailing
// allocation AFTER the fold so a collection is forced; every block was verified to throw on the
// pre-fix build and to pass after the fix. See compiler.hpp `tryEmitFolded`.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run a program; true iff it completes without throwing (asserts inside throw on failure).
static bool ok(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return true; } catch (...) { return false; }
}

int main() {
    // folded large Integer (1200 is outside the small-int intern range -> a real arena object)
    {
        KiritoVM vm; vm.setGcThreshold(1);
        CHECK(ok(vm, R"(
var big = 6 * 200
var s = "trailing allocation forces a compile-time GC after the fold"
var t = "and another"
assert big == 1200
)"));
    }
    // folded Float
    {
        KiritoVM vm; vm.setGcThreshold(1);
        CHECK(ok(vm, R"(
var f = 3.0 * 2.5
var s = "trailing allocation one"
var t = "trailing allocation two"
assert f == 7.5
)"));
    }
    // folded String (concatenation of two literals)
    {
        KiritoVM vm; vm.setGcThreshold(1);
        CHECK(ok(vm, R"(
var joined = "ab" + "cd"
var s = "trailing allocation one"
var t = "trailing allocation two"
assert joined == "abcd"
)"));
    }
    // the original field report: a folded const in the LAST statement (assert's own message string is
    // the trailing allocation that triggers the sweep)
    {
        KiritoVM vm; vm.setGcThreshold(1);
        CHECK(ok(vm, R"(
var count = 0
for s in range(6):
    count = count + 200
assert count == 6 * 200, "running total"
)"));
    }
    return RUN_TESTS();
}
