// Exhaustive coverage of the C++ facade's CONTAINER surface (value.hpp): every List/Dict/Set
// constructor, accessor, mutator, membership check, key/value/pair enumerator, and range-for
// iterator, plus their adversarial error paths (index/key out of range, empty pop, discard-of-absent,
// wrong-kind wrap). Ends with a GC pinning stress: 200-element containers built one element at a time
// under `gcThreshold(1)`, so every intermediate allocation triggers a collection the wrapper pin must
// survive.
#include <cstdint>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    KiritoVM vm;

    // ================= List =================
    {
        List xs(vm, {10, 20, 30});
        CHECK(xs.size() == 3 && !xs.empty());
        CHECK(xs[0].asInt() == 10);
        CHECK(xs[-1].asInt() == 30);                                  // negative index
        CHECK_THROWS(xs[3]);
        CHECK_THROWS(xs[-4]);

        xs.set(0, Value(vm, 11));
        xs.set(-1, Value(vm, 33));
        CHECK(xs[0].asInt() == 11 && xs[2].asInt() == 33);
        CHECK_THROWS(xs.set(9, Value(vm, 0)));

        xs.push(Value(vm, 40)).push(50);                              // chainable + template push
        CHECK(xs.size() == 5 && xs[-1].asInt() == 50);
        CHECK(xs.pop().asInt() == 50);
        CHECK(xs.size() == 4);

        CHECK(xs.contains(Value(vm, 11)));
        CHECK(!xs.contains(Value(vm, 999)));

        int64_t sum = 0;
        for (Value v : xs) sum += v.asInt();                         // range-for
        CHECK(sum == 11 + 20 + 33 + 40);

        xs.clear();
        CHECK(xs.empty());
        CHECK_THROWS(xs.pop());                                       // pop from empty

        // bulk ctor from a vector of Handles
        std::vector<Handle> hs{vm.makeInt(1), vm.makeInt(2), vm.makeInt(3)};
        CHECK(List(vm, hs).size() == 3);
        // empty ctor
        CHECK(List(vm).empty());
        // wrong-kind wrap
        CHECK_THROWS(Value(vm, 5).asList());
    }

    // ================= Dict =================
    {
        Dict d(vm, {{"a", 1}, {"b", 2}});
        CHECK(d.size() == 2 && !d.empty());
        CHECK(d["a"].asInt() == 1);
        CHECK(d[std::string_view("b")].asInt() == 2);
        CHECK(d.at(Value(vm, "a")).asInt() == 1);
        CHECK_THROWS(d.at(Value(vm, "missing")));
        CHECK_THROWS(d["missing"]);
        CHECK(d.get("missing", Value(vm, -1)).asInt() == -1);
        CHECK(d.get(Value(vm, "a"), Value(vm, -1)).asInt() == 1);
        CHECK(d.tryGet("a").has_value());
        CHECK(!d.tryGet("zzz").has_value());

        d.set("c", Value(vm, 3)).set("d", Value(vm, 4));             // chainable
        CHECK(d.size() == 4);
        CHECK(d.contains("c") && d.has("c"));
        CHECK(!d.contains("zzz"));

        CHECK(d.remove("c"));
        CHECK(!d.remove("c"));                                        // already gone -> false
        CHECK(d.size() == 3);

        CHECK(d.keys().size() == 3);
        CHECK(d.values().size() == 3);
        CHECK(d.pairs().size() == 3);

        int64_t vsum = 0;
        for (auto [k, v] : d) { (void)k; vsum += v.asInt(); }         // range-for over pairs
        CHECK(vsum == 1 + 2 + 4);

        d.clear();
        CHECK(d.empty());

        // non-string keys via the templated set()
        Dict d2(vm);
        d2.set(1, 100).set(2, 200);
        CHECK(d2.at(Value(vm, 1)).asInt() == 100);
        CHECK(d2.at(Value(vm, 2)).asInt() == 200);

        CHECK_THROWS(Value(vm, 5).asDict());
    }

    // ================= Set =================
    {
        Set s(vm, {1, 2, 2, 3});                                      // dup collapses
        CHECK(s.size() == 3);
        CHECK(s.contains(Value(vm, 2)));
        CHECK(s.contains(3));                                         // template contains
        CHECK(!s.contains(99));

        s.add(Value(vm, 4)).add(5);                                  // chainable + template add
        CHECK(s.size() == 5);
        s.add(1);                                                     // dup -> no growth
        CHECK(s.size() == 5);

        s.discard(Value(vm, 1));
        CHECK(!s.contains(1) && s.size() == 4);
        s.discard(Value(vm, 999));                                    // absent -> silent
        CHECK(s.size() == 4);

        int64_t ss = 0;
        for (Value v : s) ss += v.asInt();
        CHECK(ss == 2 + 3 + 4 + 5);
        CHECK(s.items().size() == 4);

        s.clear();
        CHECK(s.empty());
        CHECK_THROWS(Value(vm, 5).asSet());
    }

    // ================= GC pinning stress: build 200-element containers under gcThreshold(1) =======
    {
        vm.setGcThreshold(1);                                         // collect on every allocation

        List big(vm);
        for (int i = 0; i < 200; ++i) big.push(Value(vm, std::string("s") + std::to_string(i)));
        CHECK(big.size() == 200);
        CHECK(big[0].asStringRef() == "s0");
        CHECK(big[199].asStringRef() == "s199");                      // survived GC mid-build

        Dict bigd(vm);
        for (int i = 0; i < 200; ++i)
            bigd.set(Value(vm, i), Value(vm, std::string("v") + std::to_string(i)));
        CHECK(bigd.size() == 200);
        CHECK(bigd.at(Value(vm, 150)).asStringRef() == "v150");

        Set bigs(vm);
        for (int i = 0; i < 200; ++i) bigs.add(Value(vm, i));
        CHECK(bigs.size() == 200);
        CHECK(bigs.contains(199));
    }

    return RUN_TESTS();
}
