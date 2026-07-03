// Direct exercise of the Object operation-protocol slots (object.hpp) on the BUILT-IN value types.
// Embedders/extenders call these slots directly; the .ki suite drives them end-to-end, but this pins
// the C++ contract: binary/unary/getItem/setItem/iterate/length/contains/slice/hash/hashable/children
// and the base "unsupported" throws.
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    KiritoVM vm;
    auto& arena = vm.arena();

    // ---- binary / unary on Integer ----
    {
        Handle a = vm.makeInt(5), b = vm.makeInt(3);
        Object& o = arena.deref(a);
        CHECK(vm.stringify(o.binary(vm, BinOp::Add, a, b)) == "8");
        CHECK(vm.stringify(o.binary(vm, BinOp::Sub, a, b)) == "2");
        CHECK(vm.stringify(o.binary(vm, BinOp::Mul, a, b)) == "15");
        CHECK(vm.stringify(o.binary(vm, BinOp::Mod, a, b)) == "2");
        CHECK(vm.stringify(o.unary(vm, UnOp::Neg, a)) == "-5");
    }

    // ---- List: getItem / setItem / iterate / length / contains / slice / children ----
    {
        Handle lst = makeList(vm, {vm.makeInt(10), vm.makeInt(20), vm.makeInt(30)}).handle();
        Object& lo = arena.deref(lst);
        std::array<Handle, 1> k1{vm.makeInt(1)};
        CHECK(vm.stringify(lo.getItem(vm, k1)) == "20");
        lo.setItem(vm, k1, vm.makeInt(99));
        CHECK(vm.stringify(lo.getItem(vm, k1)) == "99");
        auto it = lo.iterate(vm);
        CHECK(it.has_value() && it->size() == 3);
        auto len = lo.length(vm);
        CHECK(len.has_value() && *len == 3);
        CHECK(lo.contains(vm, vm.makeInt(10)) == true);
        CHECK(lo.contains(vm, vm.makeInt(77)) == false);
        Handle sl = lo.slice(vm, vm.makeInt(0), vm.makeInt(2), vm.none());
        CHECK(vm.stringify(sl) == "[10, 99]");
        std::vector<Handle> kids;
        lo.children(kids);
        CHECK(kids.size() == 3);  // the GC/serialization contract: enumerates contained handles
    }

    // ---- Dict: getItem / length / contains / children ----
    {
        Dict d(vm);
        d.set("x", val(vm, 1)).set("y", val(vm, 2));
        Handle dh = d.build().handle();
        Object& dobj = arena.deref(dh);
        std::array<Handle, 1> kx{vm.makeString("x")};
        CHECK(vm.stringify(dobj.getItem(vm, kx)) == "1");
        auto dlen = dobj.length(vm);
        CHECK(dlen.has_value() && *dlen == 2);
        CHECK(dobj.contains(vm, vm.makeString("y")) == true);
        std::vector<Handle> dkids;
        dobj.children(dkids);
        CHECK(dkids.size() >= 2);  // keys + values are reachable children
    }

    // ---- String: getItem (code-point), length, contains, iterate ----
    {
        Handle s = vm.makeString("aéz");  // a é z  -> 3 code points
        Object& so = arena.deref(s);
        auto slen = so.length(vm);
        CHECK(slen.has_value() && *slen == 3);
        std::array<Handle, 1> i1{vm.makeInt(1)};
        CHECK(vm.stringify(so.getItem(vm, i1)) == "é");
        CHECK(so.contains(vm, vm.makeString("z")) == true);
        auto sit = so.iterate(vm);
        CHECK(sit.has_value() && sit->size() == 3);
    }

    // ---- hashable / hash ----
    {
        CHECK(arena.deref(vm.makeInt(7)).hashable() == true);
        CHECK(arena.deref(vm.makeString("k")).hashable() == true);
        CHECK(arena.deref(vm.makeBool(true)).hashable() == true);
        (void)arena.deref(vm.makeInt(7)).hash();   // does not throw
        Handle lst = makeList(vm, {vm.makeInt(1)}).handle();
        CHECK(arena.deref(lst).hashable() == false);
        CHECK_THROWS(arena.deref(lst).hash());      // unhashable type
    }

    // ---- base "unsupported" throws on a scalar ----
    {
        Handle n = vm.makeInt(5);
        Object& o = arena.deref(n);
        std::array<Handle, 1> k{vm.makeInt(0)};
        CHECK_THROWS(o.call(vm, {}));            // not callable
        CHECK_THROWS(o.getItem(vm, k));          // not indexable
        CHECK_THROWS(o.iterate(vm));             // not iterable
        CHECK_THROWS(o.length(vm));              // no length
        CHECK_THROWS(o.getAttr(vm, n, "nope"));  // no attribute
        Handle noneh = vm.none();
        CHECK_THROWS(arena.deref(noneh).contains(vm, n));  // `in` unsupported on None
    }

    // ---- equals on built-ins (structural) ----
    {
        CHECK(arena.deref(vm.makeInt(5)).equals(arena, arena.deref(vm.makeInt(5))) == true);
        CHECK(arena.deref(vm.makeInt(5)).equals(arena, arena.deref(vm.makeInt(6))) == false);
        Handle l1 = makeList(vm, {vm.makeInt(1), vm.makeInt(2)}).handle();
        Handle l2 = makeList(vm, {vm.makeInt(1), vm.makeInt(2)}).handle();
        CHECK(arena.deref(l1).equals(arena, arena.deref(l2)) == true);
    }

    return RUN_TESTS();
}
