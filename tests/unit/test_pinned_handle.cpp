// test_pinned_handle.cpp — exhaustive coverage of PinnedHandle (value.hpp): the owning, copy/move
// RAII GC root a host stores in a long-lived C++ object. Verifies it survives collection, that its
// pin is refcounted (copies own independent pins; the underlying object lives until the LAST pin is
// gone), that move transfers the pin and empties the source, that copy/move-assignment release the
// old pin, that self-assignment and reset()/empty are safe, that it drives the exact real-world
// scenario (a compiled function surviving heavy allocation churn), and that it behaves in a
// std::vector (element moves during growth stay balanced).
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    // ---- a pinned handle survives an explicit collection (auto-GC off) ----
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        PinnedHandle p(vm, vm.makeString("kept"));
        vm.collectGarbage();
        CHECK(p.pinned());
        CHECK(vm.stringify(p) == "kept");            // operator Handle -> stringify
        CHECK(p.value().asStringRef() == "kept");    // .value() wraps for the ergonomic API
    }

    // ---- default-constructed is empty; reset() and destruction of an empty are safe ----
    {
        PinnedHandle e;
        CHECK(!e.pinned());
        e.reset();
        e.reset();                                   // idempotent
        // destructor of an empty runs at end of scope — must not touch a VM
    }

    // ---- construct from a Value; accessors (vm/handle/value) ----
    {
        KiritoVM vm;
        Value v(vm, "fromvalue");
        PinnedHandle p{v};
        CHECK(&p.vm() == &vm);
        CHECK(p.handle() == v.handle());
        CHECK(p.value().asStringRef() == "fromvalue");
    }

    // ---- copies hold INDEPENDENT pins: object lives until the LAST pin is released ----
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        Handle h = vm.makeString("shared");
        PinnedHandle a(vm, h);
        PinnedHandle b = a;                          // copy -> h pinned twice
        a.reset();
        vm.collectGarbage();
        CHECK(vm.stringify(h) == "shared");          // still pinned by b
        std::size_t before = vm.liveCount();
        b.reset();                                   // last pin gone
        vm.collectGarbage();
        CHECK(vm.liveCount() < before);              // now reclaimed
    }

    // ---- move transfers the pin and leaves the source empty (and reset-safe) ----
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        PinnedHandle a(vm, vm.makeString("moveme"));
        PinnedHandle b = std::move(a);
        CHECK(!a.pinned() && b.pinned());
        a.reset();                                   // safe on a moved-from empty
        PinnedHandle c;
        c = std::move(a);                            // move-assign from empty
        CHECK(!c.pinned());
        vm.collectGarbage();
        CHECK(b.value().asStringRef() == "moveme");  // still rooted by exactly one pin
    }

    // ---- copy-assignment releases the old pin, takes the new one ----
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        PinnedHandle a(vm, vm.makeString("AAA"));
        PinnedHandle b(vm, vm.makeString("BBB"));
        std::size_t before = vm.liveCount();
        a = b;                                       // a drops AAA, takes BBB
        CHECK(a.value().asStringRef() == "BBB");
        CHECK(b.value().asStringRef() == "BBB");     // b unaffected
        vm.collectGarbage();
        CHECK(vm.liveCount() < before);              // AAA reclaimed (only a had pinned it)
    }

    // ---- assigning an EMPTY over a pinned one releases the pin ----
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        PinnedHandle p(vm, vm.makeString("droptome"));
        std::size_t before = vm.liveCount();
        PinnedHandle e;
        p = e;                                       // p releases its handle, becomes empty
        CHECK(!p.pinned());
        vm.collectGarbage();
        CHECK(vm.liveCount() < before);
    }

    // ---- self copy-assignment and self move-assignment are safe (no double-unpin, stays pinned) ----
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        PinnedHandle p(vm, vm.makeString("self"));
        PinnedHandle& alias = p;                     // alias so it isn't a literal p=p (self-assign warning)
        p = alias;
        vm.collectGarbage();
        CHECK(p.pinned() && p.value().asStringRef() == "self");
        p = std::move(alias);
        vm.collectGarbage();
        CHECK(p.pinned() && p.value().asStringRef() == "self");
    }

    // ---- refcount balance: pinning the SAME handle twice needs two releases ----
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        Handle h = vm.makeString("twice");
        PinnedHandle a(vm, h), b(vm, h);
        a.reset();
        b.reset();
        std::size_t before = vm.liveCount();
        vm.collectGarbage();
        CHECK(vm.liveCount() < before);              // both pins gone -> reclaimed
    }

    // ---- pinning interned singletons (small int / Bool / None) is harmless ----
    {
        KiritoVM vm;
        PinnedHandle i(vm, vm.makeInt(5));
        PinnedHandle b(vm, vm.makeBool(true));
        PinnedHandle n(vm, vm.none());
        vm.collectGarbage();
        CHECK(i.value().asInt() == 5);
        CHECK(b.value().asBool() == true);
        CHECK(n.value().isNone());
        i.reset(); b.reset(); n.reset();             // unpinning an interned handle is a no-op
        CHECK(vm.stringify(vm.makeInt(5)) == "5");   // VM still healthy
    }

    // ---- operator Handle works with the raw protocol (arena().deref) ----
    {
        KiritoVM vm;
        PinnedHandle fn(vm, vm.runSource("Function(x): return x + 100\n"));
        std::array<Handle, 1> args{vm.makeInt(1)};
        Handle r = vm.arena().deref(fn).call(vm, args);
        CHECK(vm.stringify(r) == "101");
    }

    // ---- the real-world scenario: a compiled function survives heavy allocation churn ----
    // (This is exactly the embed_physics_step bug: a bare Handle would be swept mid-run.)
    {
        KiritoVM vm;
        PinnedHandle fn(vm, vm.runSource("Function(x): return x * 2\n"));
        vm.setGcThreshold(1);                        // collect on every allocation
        vm.runSource("var i = 0\nwhile i < 5000:\n    var t = [i, i + 1]\n    i = i + 1\n");
        Value r = fn.value().call({Value(vm, 21)});
        CHECK(r.asInt() == 42);                      // fn survived thousands of collections
    }

    // ---- PinnedHandle in a std::vector: element moves during growth stay balanced ----
    {
        KiritoVM vm;
        vm.setGcEnabled(false);
        std::vector<PinnedHandle> pins;
        for (int k = 0; k < 100; ++k)
            pins.emplace_back(vm, vm.makeString("v" + std::to_string(k)));   // growth moves elements
        vm.collectGarbage();
        CHECK(pins.front().value().asStringRef() == "v0");
        CHECK(pins.back().value().asStringRef() == "v99");
        std::size_t before = vm.liveCount();
        pins.clear();                                // every element's dtor unpins
        vm.collectGarbage();
        CHECK(vm.liveCount() < before);              // the 100 strings reclaimed
    }

    return RUN_TESTS();
}
