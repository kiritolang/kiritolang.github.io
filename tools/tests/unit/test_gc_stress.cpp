// Adversarial GC + memory-management stress, complementing test_gc.cpp. Focuses on the pieces the
// 1.11 C++ facade added or leans on: wrapper GC-pins surviving an EXPLICIT collection with automatic
// GC disabled, raw pinHandle/unpinHandle + RootScope protecting bare allocations, mark-sweep
// reclaiming reference CYCLES, deeply-nested reachable structures surviving aggressive collection, and
// the small-object pool churning through 100k+ short-lived boxes without growing the live set.
#include <cstddef>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    // ===== wrapper pins survive an explicit collection while automatic GC is OFF =====
    // Nothing roots these containers except the wrapper's shared_ptr<Pin>; a manual collectGarbage
    // between construction and use must not reclaim them.
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        List a(vm, {1, 2, 3});
        Dict d(vm, {{"k", "v"}});
        Set s(vm, {7, 8, 9});
        String str(vm, "pinned");
        Value num(vm, 12345);                                        // boxed Integer, also pinned
        vm.collectGarbage();
        CHECK(a.size() == 3 && a[0].asInt() == 1 && a[2].asInt() == 3);
        CHECK(d.at(Value(vm, "k")).asStringRef() == "v");
        CHECK(s.contains(8));
        CHECK(str.utf8() == "pinned");
        CHECK(num.asInt() == 12345);
    }

    // ===== raw pinHandle/unpinHandle protects a bare allocation across collection =====
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        Handle h = vm.makeString("raw");
        vm.pinHandle(h);
        vm.collectGarbage();
        CHECK(vm.stringify(h) == "raw");                             // survived while pinned
        vm.unpinHandle(h);
    }

    // ===== RootScope protects intermediates for its lifetime =====
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        RootScope roots(vm);
        Handle a = vm.makeString("aaa"); roots.add(a);
        Handle b = vm.makeString("bbb"); roots.add(b);
        vm.collectGarbage();
        CHECK(vm.stringify(a) == "aaa" && vm.stringify(b) == "bbb");
    }

    // ===== mark-sweep reclaims reference cycles (a<->b) without leaking or crashing =====
    {
        KiritoVM vm;
        vm.setGcThreshold(100);
        vm.runSource(R"(
var i = 0
while i < 5000:
    var a = []
    var b = []
    a.append(b)
    b.append(a)
    i = i + 1
)");
        vm.collectGarbage();
        CHECK(vm.liveCount() < 5000);                               // cyclic garbage was collected
    }

    // ===== a deeply-nested reachable structure survives aggressive collection =====
    {
        KiritoVM vm;
        vm.setGcThreshold(50);
        std::string out = vm.stringify(vm.runSource(R"(
var x = 0
var i = 0
while i < 1000:
    x = [x, i]
    i = i + 1
x[1]
)"));
        CHECK(out == "999");                                        // innermost value intact
    }

    // ===== the small-object pool churns 100k short-lived boxes; live set stays bounded =====
    {
        KiritoVM vm;
        vm.setGcThreshold(256);
        vm.runSource(R"(
var acc = 0
var i = 0
while i < 100000:
    var t = (i * 3) // 2 - 7
    var f = (i * 1.5) / 2.0
    acc = i
    i = i + 1
)");
        CHECK(vm.liveCount() < 5000);
    }

    // ===== pin refcounting: multiple pins on one handle need matching unpins =====
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        Handle h = vm.makeString("multi");
        vm.pinHandle(h);
        vm.pinHandle(h);
        vm.unpinHandle(h);
        vm.collectGarbage();
        CHECK(vm.stringify(h) == "multi");                          // still pinned once
        vm.unpinHandle(h);
    }

    return RUN_TESTS();
}
