#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    // A loop that allocates a lot of garbage must not grow memory without bound.
    {
        KiritoVM vm;
        vm.setGcThreshold(1000);  // collect often to stress the collector
        vm.runSource(R"(
var i = 0
while i < 200000:
    var x = i * 2
    i = i + 1
)");
        // After churning ~200k temporaries with a small threshold, the live set stays tiny.
        std::size_t live = vm.liveCount();
        CHECK(live < 5000);
    }

    // Reachable values survive collection and remain correct.
    {
        KiritoVM vm;
        vm.setGcThreshold(500);
        std::string out = vm.stringify(vm.runSource(R"(
var keep = [1, 2, 3]
var i = 0
while i < 50000:
    var junk = [i, i + 1]
    i = i + 1
keep[0] + keep[1] + keep[2]
)"));
        CHECK(out == "6");
    }

    // A closure's captured environment survives GC across many allocations.
    {
        KiritoVM vm;
        vm.setGcThreshold(500);
        std::string out = vm.stringify(vm.runSource(R"(
var makeAdder = Function(n):
    return Function(x):
        return x + n
var add10 = makeAdder(10)
var i = 0
while i < 50000:
    var junk = i * i
    i = i + 1
add10(5)
)"));
        CHECK(out == "15");
    }

    // A bound method (list.append) keeps its receiver alive even under heavy collection.
    {
        KiritoVM vm;
        vm.setGcThreshold(200);
        std::string out = vm.stringify(vm.runSource(R"(
var data = []
var i = 0
while i < 1000:
    data.append(i)
    var junk = [i, i, i]
    i = i + 1
len(data)
)"));
        CHECK(out == "1000");
    }

    // Explicit collection works and reclaims unreachable values.
    {
        KiritoVM vm;
        vm.setGcEnabled(false);  // no automatic GC; we drive it manually
        vm.runSource("var a = [1, 2, 3]");
        std::size_t before = vm.liveCount();
        vm.runSource("var b = [4, 5, 6, 7, 8]");  // each runSource makes a throwaway module scope
        vm.collectGarbage();
        // 'a' and 'b' live in throwaway module scopes (unreachable after runSource) so they are
        // reclaimed; only VM-level roots remain, leaving fewer live objects than at the peak.
        CHECK(vm.liveCount() <= before);
    }

    return RUN_TESTS();
}
