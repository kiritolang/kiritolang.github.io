// Generational GC mechanics, asserted directly through the arena/VM API. The generational collector
// (young/old generations, cheap minor + full major collection, a write barrier + remembered set for
// old->young edges) must reclaim churn cheaply WITHOUT ever freeing a still-reachable young object —
// a missed write barrier is a use-after-free. These tests pin the mechanics; the .ki golden +
// adversarial suites and the ASan/gc-every-alloc soak are the end-to-end barrier-completeness proof.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    // ===== a fresh alloc is YOUNG; a rooted survivor is PROMOTED; the nursery empties on a minor =====
    {
        KiritoVM vm;
        vm.setGcEnabled(false);                 // drive collection manually
        Handle h = vm.makeString("keep");
        vm.pinHandle(h);                        // root it
        CHECK(vm.youngCount() >= 1);            // h (and construction objects) are young
        vm.minorCollect();                      // h survives -> promoted to old
        CHECK(vm.youngCount() == 0);            // kPromoteAge==1: every survivor promoted, nursery empty
        CHECK(vm.stringify(h) == "keep");       // still alive and correct
        vm.unpinHandle(h);
    }

    // ===== an unrooted young object is reclaimed by a minor =====
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        Handle tmp = vm.makeString("garbage");  // a bare Handle is NOT a GC root
        vm.minorCollect();
        CHECK_THROWS(vm.arena().deref(tmp));    // reclaimed -> stale generation
    }

    // ===== minor isolation: an old object is neither marked-dead nor swept by a minor =====
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        Handle h = vm.makeString("old");
        vm.pinHandle(h);
        vm.minorCollect();                      // promote h to old
        vm.unpinHandle(h);                      // h is now unreachable, but OLD
        vm.minorCollect();                      // a minor does NOT touch the old generation
        CHECK(vm.stringify(h) == "old");        // still alive (floating old garbage)
        vm.collectGarbage();                    // a MAJOR reclaims it
        CHECK_THROWS(vm.arena().deref(h));
    }

    // ===== WRITE BARRIER, one focused test per mutable container type: an OLD container gains a YOUNG
    // value reachable ONLY through it; a minor must keep that value alive (the barrier's core promise).
    // The pushed value's temporary wrapper unpins immediately, so the container is its sole referent. =====
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        List xs(vm, {});
        vm.minorCollect();                      // promote the list to old
        xs.push(Value(vm, 918273));             // barriered store; 918273 is not an interned small int
        vm.minorCollect();                      // the young Integer survives only if xs was remembered
        CHECK(xs.size() == 1 && xs[0].asInt() == 918273);
    }
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        Dict d(vm, {});
        vm.minorCollect();                      // old
        d.set(Value(vm, "k"), Value(vm, 654321));
        vm.minorCollect();
        CHECK(d.at(Value(vm, "k")).asInt() == 654321);
    }
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        Set s(vm, {});
        vm.minorCollect();                      // old
        s.add(Value(vm, 777001));
        vm.minorCollect();
        CHECK(s.contains(777001));
    }

    // ===== barrier via the Instance, Module, and EnvValue (global) paths — driven through Kirito, run
    // under GC-on-every-alloc so a promoted container repeatedly gains fresh young members. =====
    {
        KiritoVM vm;
        vm.setGcThreshold(1);                   // collect on EVERY allocation
        // an instance attribute reassigned every iteration; a module-level (global-ish) list filled;
        // a Dict grown — all old after the first collections, all gaining young values each step.
        Handle out = vm.runSource(R"(
class Acc:
 var _init_ = Function(self): self.total = 0
 var bump = Function(self, x): self.total = self.total + x
var a = Acc()
var xs = []
var d = {}
var i = 0
while i < 300:
    a.bump(i)
    xs.append(i)
    d[i] = i
    i = i + 1
String(a.total) + "," + String(len(xs)) + "," + String(len(d)) + "," + String(xs[299])
)");
        CHECK(vm.stringify(out) == "44850,300,300,299");   // 44850 == sum(0..299)
    }

    // ===== kPromoteAge==1 trap: a young object referenced ONLY by an old object survives REPEATED
    // minors (guards the remembered-set clear/coupling — after promotion the edge is old->old). =====
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        List xs(vm, {});
        vm.minorCollect();                      // xs old
        xs.push(Value(vm, 424242));
        vm.minorCollect();                      // value promoted (survived via the remembered set)
        vm.minorCollect();                      // value now old, still reachable from xs
        vm.minorCollect();
        CHECK(xs[0].asInt() == 424242);
    }

    // ===== the two RAW-write traps under GC-on-every-alloc: (a) a multi-method class built + instanced
    // (BuildClass mid-build promotion + the owned-clone method rebind), (b) a large cyclic instance
    // graph rebuilt by dump.loads (deserialize wires young values into promoted containers). =====
    {
        KiritoVM vm;
        vm.setGcThreshold(1);
        Handle out = vm.runSource(R"(
class Widget:
 var _init_ = Function(self, n): self.n = n
 var a = Function(self): return self.n + 1
 var b = Function(self): return self.n * 2
 var c = Function(self): return self.n - 3
var w = Widget(10)
String(w.a()) + "," + String(w.b()) + "," + String(w.c())
)");
        CHECK(vm.stringify(out) == "11,20,7");
    }
    {
        KiritoVM vm;
        vm.setGcThreshold(1);
        Handle out = vm.runSource(R"(
var d = import("dump")
class Node:
 var _init_ = Function(self, v): self.v = v
var a = Node(1)
var b = Node(2)
a.peer = b
b.peer = a                       # a cycle across two instances
var big = []
var i = 0
while i < 50:
    big.append(Node(i))
    i = i + 1
var blob = d.dumps([a, big])
var back = d.loads(blob)
String(back[0].v) + "," + String(back[0].peer.v) + "," + String(back[1][49].v) + "," + String(len(back[1]))
)");
        CHECK(vm.stringify(out) == "1,2,49,50");
    }

    // ===== roots keep young alive across a minor: RootScope, pinHandle, interned small ints =====
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        RootScope rs(vm);
        Handle a = rs.add(vm.makeString("rooted"));
        vm.minorCollect();
        CHECK(vm.stringify(a) == "rooted");     // RootScope kept it
        // interned small ints are permanent roots and survive both collectors
        Handle five = vm.makeInt(5);
        vm.minorCollect();
        vm.collectGarbage();
        CHECK(vm.stringify(five) == "5");
    }

    // ===== dangling-handle diagnostic surfaces sooner under frequent minors (the embedder failure
    // mode): a mis-rooted bare handle dereffed after a collection throws the catchable stale-gen error. =====
    {
        KiritoVM vm;
        vm.setGcThreshold(1);
        Handle h = vm.makeString("ephemeral");  // deliberately NOT rooted
        for (int i = 0; i < 40; ++i) vm.makeString("churn");  // force minors
        bool threw = false;
        try { (void)vm.arena().deref(h); } catch (const KiritoError& e) {
            threw = true;
            CHECK(std::string(e.what()).find("dangling handle") != std::string::npos);
        }
        CHECK(threw);
    }

    // ===== GcPauseScope suspends BOTH minor and major collection for its lifetime =====
    {
        KiritoVM vm;
        vm.setGcThreshold(1);                   // would collect on every alloc if enabled
        {
            GcPauseScope pause(vm);
            Handle keep = vm.makeString("survives");
            for (int i = 0; i < 200; ++i) vm.makeString("junk");
            CHECK(vm.stringify(keep) == "survives");   // nothing collected while paused
        }
        CHECK(vm.gcEnabled());
    }

    // ===== the counters: churn drives many cheap minors and rare majors (the --gc-stats yardstick) =====
    {
        KiritoVM vm;
        vm.setGcThreshold(1000);
        vm.runSource(R"(
var i = 0
while i < 200000:
    var x = [i, i * 2]
    i = i + 1
)");
        CHECK(vm.gcMinorCount() > 0);
        CHECK(vm.gcMajorCount() > 0);
        CHECK(vm.gcMinorCount() > vm.gcMajorCount());   // minors are the frequent, cheap collection
        CHECK(vm.liveCount() < 5000);                   // churn reclaimed; live set stays tiny
    }

    // ===== structural equality + reachability survive a full generational lifecycle unchanged =====
    {
        KiritoVM vm;
        vm.setGcThreshold(200);
        std::string out = vm.stringify(vm.runSource(R"(
var keep = [1, [2, 3], {"k": 4}]
var i = 0
while i < 50000:
    var junk = [i, {"x": i}]
    i = i + 1
String(keep[0]) + String(keep[1][1]) + String(keep[2]["k"])
)"));
        CHECK(out == "134");
    }

    return RUN_TESTS();
}
