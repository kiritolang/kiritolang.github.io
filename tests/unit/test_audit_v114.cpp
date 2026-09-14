// Regression tests for the v1.14 audit (.audit/v1.14/) that assert directly on the C++ embedding
// API (Value/String/List operator results, raw Object allocation) rather than on Kirito script
// behaviour, so they cannot be expressed as a .ki golden. The script-observable findings from this
// audit round were converted to tests/scripts/unit_audit_v114.ki (registered to also run under
// `--gc-threshold 1`, the same aggressive cadence the A19-1/A19-2 block below forces manually).
#include <cstdint>
#include <new>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    // === A19-1 / A19-2 (MEDIUM, embedding memory): Value's arithmetic/unary operators, call,
    // getAttr, at (getItem) and List::pop wrapped their fresh, not-yet-rooted result in the
    // NON-pinning Value(vm, Handle) ctor, so a GC between the op and first use swept it (dangling
    // handle / ASan UAF). They now `adopting`-pin the result. Force GC on every allocation and use
    // each result AFTER many intervening allocations — under ASan this is the real gate. ===
    {
        KiritoVM vm;
        vm.installStandardLibrary();
        vm.setGcThreshold(1);                       // collect on every allocation
        auto churn = [&] { for (int i = 0; i < 64; ++i) vm.makeString("gc-churn-padding-string"); };

        Value a(vm, 2.5), b(vm, 4.0);
        Value sum = a + b;                          // fresh Float (A19-1)
        Value neg = -a;                             // fresh Float, unary (A19-1)
        churn();
        CHECK(sum.asFloat() == 6.5);                // swept-and-reused if unpinned
        CHECK(neg.asFloat() == -2.5);

        String s1(vm, "foo"), s2(vm, "bar");
        Value cat = s1 + s2;                        // fresh String concat
        churn();
        CHECK(cat.str() == "foobar");

        List xs(vm, {10, 20, 30});
        Value popped = xs.pop();                    // orphaned element (A19-2)
        churn();
        CHECK(popped.asInt() == 30);

        String hello(vm, "hello");
        Value ch = hello.at(1);                     // fresh 1-char String via getItem (A19-2)
        churn();
        CHECK(ch.str() == "e");
    }

    // === A07-1 (MEDIUM, latent UB): declaring a member Object::operator new HID the global aligned
    // allocation functions, so an over-aligned Object subclass would be routed through the 16-aligned
    // small-object pool and constructed under-aligned (UB, sanitizer-invisible). The aligned
    // operator new/delete overloads re-expose the aligned path. This test both COMPILES (the overload
    // exists) and checks it hands back correctly-aligned storage. ===
    {
        void* p = Object::operator new(64, std::align_val_t(32));
        CHECK((reinterpret_cast<std::uintptr_t>(p) % 32) == 0);
        Object::operator delete(p, std::align_val_t(32));
        void* q = Object::operator new(128, std::align_val_t(64));
        CHECK((reinterpret_cast<std::uintptr_t>(q) % 64) == 0);
        Object::operator delete(q, 128, std::align_val_t(64));
    }

    return RUN_TESTS();
}
