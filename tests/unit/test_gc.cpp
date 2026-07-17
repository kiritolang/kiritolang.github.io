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

    // Two VMs on ONE thread: each write barrier must consult the arena that owns the container, not
    // whichever VM was constructed last. The barrier used to find its arena through activeVM(), so
    // mutating A's old global while B existed asked B about A's handle — a spurious dangling-handle
    // throw, or (when the slot and generation happened to collide, which they readily do for low
    // slots) a wrong young/old answer that skipped remember() and let A's next minor free a value A
    // could still reach. The embedding API explicitly allows coexisting VMs, so this is reachable.
    // v1.15 A05-1.
    {
        KiritoVM a;
        a.installStandardLibrary();
        a.setGcThreshold(1);  // every allocation collects: promote A's globals, then hammer the barrier
        a.runSource("var seed = 1");
        a.collectGarbage();
        a.collectGarbage();  // A's global scope is now OLD (survived a major)

        KiritoVM b;  // ...and now B is the most recently constructed VM on this thread
        b.installStandardLibrary();
        b.setGcThreshold(1);
        b.runSource("var other = 2");

        // Store a FRESH (young) value into A's now-old global scope while B is live. The barrier
        // must enrol A's scope in A's remembered set, or the next minor reclaims `kept`.
        for (int i = 0; i < 50; ++i) {
            a.registerGlobal("kept" + std::to_string(i), a.makeString("value-" + std::to_string(i)));
            b.registerGlobal("bkept" + std::to_string(i), b.makeString("b-" + std::to_string(i)));
        }
        a.collectGarbage();
        b.collectGarbage();
        // Every value must still be reachable and intact in ITS OWN VM (no cross-arena confusion).
        for (int i = 0; i < 50; ++i) {
            CHECK(a.stringify(a.runSource("kept" + std::to_string(i))) == "value-" + std::to_string(i));
            CHECK(b.stringify(b.runSource("bkept" + std::to_string(i))) == "b-" + std::to_string(i));
        }
        // An instance attribute write (InstanceValue::setAttr) takes the same path. The instance has
        // to be a global to outlive its runSource's throwaway module scope.
        a.registerGlobal("box", a.runSource("class Box:\n    var _init_ = Function(self): self.v = 0\nBox()"));
        a.collectGarbage();
        a.collectGarbage();  // box is old now
        CHECK(a.stringify(a.runSource("box.v = \"fresh young string\"\nbox.v")) == "fresh young string");
        a.collectGarbage();
        CHECK(a.stringify(a.runSource("box.v")) == "fresh young string");
    }

    return RUN_TESTS();
}
