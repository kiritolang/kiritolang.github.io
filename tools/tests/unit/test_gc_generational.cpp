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

    {
        // List::set (setElem) — the overwrite path, distinct from push's append.
        KiritoVM vm;
        vm.setGcEnabled(false);
        List xs(vm, {0});
        vm.minorCollect();                      // old
        xs.set(0, Value(vm, 313373));           // barriered element overwrite
        vm.minorCollect();
        CHECK(xs[0].asInt() == 313373);
    }
    {
        // The C++ API's setAttr, which routes through InstanceValue::setAttr — the same barrier the
        // interpreter uses, reached from a host instead. (v1.15 A05-1 moved it onto the caller's arena.)
        KiritoVM vm;
        vm.setGcEnabled(false);
        // A host holds the instance the documented way — a PinnedHandle, the owning GC root.
        PinnedHandle inst(vm, vm.runSource("class B:\n    var _init_ = Function(self): self.v = 0\nB()"));
        Value box(vm, inst.value());
        vm.minorCollect();
        vm.minorCollect();                      // the instance is old now
        box.setAttr("v", Value(vm, "young attribute value"));
        vm.minorCollect();                      // the young String survives only if the barrier fired
        CHECK(box.getAttr("v").asString() == "young attribute value");
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

    // ===== A value must be rooted the MOMENT it is made, not once its owner is arena-reachable.
    // Every case below builds a container (or a method signature) OUTSIDE the arena and allocates
    // into it: until that container is itself alloc'd, nothing traces what it holds, so the next
    // allocation collects the previous contents and the finished object hands out a dangling handle.
    // Each of these threw "dangling handle (stale generation)" before the fix; all are invisible at
    // any normal threshold, and the shape/index ones only bite past the small-int intern range —
    // which is why five audit rounds of small-value tests never saw them. (v1.15 A19-1.)
    {
        KiritoVM vm;
        vm.installStandardLibrary();
        vm.setGcThreshold(1);  // collect on EVERY allocation: makes the race deterministic
        // A native method's DEFAULTS (compare's rel_tol/abs_tol) — only reachable when a caller
        // omits the argument, so an all-positional call papered over it.
        CHECK(vm.stringify(vm.runSource("(5).compare(5)")) == "True");
        CHECK(vm.stringify(vm.runSource("(5).compare(5, rel_tol = -1.0)")) == "True");
        CHECK(vm.stringify(vm.runSource("(1.0).compare(1.5, rel_tol = 0.0)")) == "False");
        CHECK(vm.stringify(vm.runSource(
            "var T = import(\"tensor\")\nT.ones([2]).compare(T.ones([2]))")) == "True");
        CHECK(vm.stringify(vm.runSource(
            "var c = import(\"complex\")\nc.Complex(1, 2).compare(c.Complex(1, 2))")) == "True");
        CHECK(vm.stringify(vm.runSource(
            "var m = import(\"matrix\")\nm.identity(2).compare(m.identity(2))")) == "True");
        // An Integer past the intern range, held only by a not-yet-alloc'd List.
        CHECK(vm.stringify(vm.runSource(
            "var T = import(\"tensor\")\nString(T.zeros([300, 2]).shape())")) == "[300, 2]");
        CHECK(vm.stringify(vm.runSource(
            "var m = import(\"matrix\")\nString(m.zeros(300, 2).shape())")) == "[300, 2]");
        CHECK(vm.stringify(vm.runSource(
            "var c = import(\"complex\")\nString(c.zeros(300, 2).shape())")) == "[300, 2]");
        // enumerate's index Integer, likewise — ordinary code, past index 255.
        CHECK(vm.stringify(vm.runSource(
            "var xs = []\nvar i = 0\nwhile i < 300:\n    xs.append(i)\n    i = i + 1\n"
            "var last = None\nfor p in enumerate(xs):\n    last = p\nString(last)")) == "[299, 299]");
        // tensor.nonzero's per-axis tensors: each one collected the last.
        CHECK(vm.stringify(vm.runSource(
            "var T = import(\"tensor\")\nvar nz = T.nonzero(T.Tensor([[0.0, 5.0], [7.0, 0.0]]))\n"
            "String(nz[0].tolist()) + String(nz[1].tolist())")) == "[0.0, 1.0][1.0, 0.0]");
        // A user callback allocates, so its ARGUMENT must be rooted. The Complex branch of
        // tensor.apply always did; the Float branch did not.
        CHECK(vm.stringify(vm.runSource(
            "var T = import(\"tensor\")\n"
            "String(T.Tensor([1.0, 4.0, 9.0]).apply(Function(x): return x * 2.0).tolist())"))
            == "[2.0, 8.0, 18.0]");
        // Dict(vm, {...}): the value's allocation collected the key. (net.urlsplit, and any native
        // building a Dict from an initializer list.)
        CHECK(vm.stringify(vm.runSource(
            "var net = import(\"net\")\nnet.urlsplit(\"http://example.com:8080/p?q=1#f\")[\"host\"]"))
            == "example.com");
        // Dict::set(key, <fresh Handle>): allocating the key collected the caller's value.
        CHECK(vm.stringify(vm.runSource(
            "var re = import(\"regex\")\n"
            "String(re.match(\"(?P<user>[a-z]+)@(?P<host>[a-z.]+)\", \"ada@kirito.dev\").groupdict())"))
            == "{'user': 'ada', 'host': 'kirito.dev'}");
        // A module member's DEFAULTS are built as call-site arguments, so they collect each other
        // while the signature is still being evaluated — and setup() runs lazily, on first import,
        // by which time a threshold may be set. Only a caller who OMITS the argument sees it.
        CHECK(vm.stringify(vm.runSource("var net = import(\"net\")\nnet.socket(type = \"dgram\").type"))
            == "dgram");
        CHECK(vm.stringify(vm.runSource("var net = import(\"net\")\nlen(inspect(net)) > 0")) == "True");
    }

    // ===== A19-2: the WRITE BARRIER, not rooting. json's array() appended with a raw
    // elems.push_back, skipping the barrier. The list is rooted across the recursive parse, so a
    // collection PROMOTES it; an old list silently gaining a young element is never enrolled in the
    // remembered set, and the next minor frees a nested array that is still perfectly reachable.
    // Distinctive shape: the value is fine until a GC happens AFTER it is built — an immediate
    // `String(json.loads(...))` looked correct, which is how this survived five rounds. =====
    {
        KiritoVM vm;
        vm.installStandardLibrary();
        vm.setGcThreshold(1);
        CHECK(vm.stringify(vm.runSource(
            "var json = import(\"json\")\nvar p = json.loads(\"[1, [2]]\")\nString(p) + String(p[1])"))
            == "[1, [2]][2]");
        CHECK(vm.stringify(vm.runSource(
            "var json = import(\"json\")\njson.loads(\"[1, [2, [3, [4]]]]\") == [1, [2, [3, [4]]]]"))
            == "True");
        CHECK(vm.stringify(vm.runSource(
            "var json = import(\"json\")\nString(json.loads(\"{\\\"a\\\": {\\\"b\\\": [1, 2]}}\"))"))
            == "{'a': {'b': [1, 2]}}");
    }

    return RUN_TESTS();
}
