// The ergonomic C++ value API (value.hpp): reading Kirito values via the `Value` facade and `Args`,
// and building them with the List/Dict/Set builders and `val(...)` — the default way to author a
// native module against the built-in types (a new NativeClass is the fallback).
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    KiritoVM vm;

    // --- build values from C++ primitives ---
    CHECK(Value(vm, 42).asInt() == 42);
    CHECK(Value(vm, 3.5).asFloat() == 3.5);
    CHECK(Value(vm, true).asBool() == true);
    CHECK(Value(vm, std::string("hi")).asStringRef() == "hi");
    CHECK(Value::None(vm).isNone());

    // --- type queries ---
    CHECK(Value(vm, 1).isInt() && Value(vm, 1).isNumber());
    CHECK(Value(vm, 1.0).isFloat() && Value(vm, 1.0).isNumber());
    CHECK(Value(vm, "x").isString());
    CHECK(Value(vm, false).isBool());

    // --- typed-read errors are clear ---
    bool threw = false;
    try { Value(vm, "not a number").asInt("arg"); } catch (const KiritoError& e) {
        threw = std::string(e.what()).find("expected Integer") != std::string::npos;
    }
    CHECK(threw);

    // asFloat accepts Integer or Float
    CHECK(Value(vm, 7).asFloat() == 7.0);

    // --- List builder + reads ---
    Value lst = List(vm).add(1).add(2).add(3).add("four").build();
    CHECK(lst.isList());
    CHECK(lst.len() == 4);
    CHECK(lst.at(0).asInt() == 1);
    CHECK(lst.at(-1).asStringRef() == "four");        // negative index
    auto items = lst.items();
    CHECK(items.size() == 4 && items[1].asInt() == 2);
    CHECK(vm.stringify(lst) == "[1, 2, 3, 'four']");

    // makeList from handles
    Value lst2 = List(vm, {vm.makeInt(10), vm.makeInt(20)});
    CHECK(lst2.len() == 2 && lst2.at(1).asInt() == 20);

    // --- Dict builder + reads ---
    Value d = Dict(vm).set("name", "Ada").set("age", 36).set("score", 9.5).build();
    CHECK(d.isDict());
    CHECK(d.get("name").asStringRef() == "Ada");
    CHECK(d.get("age").asInt() == 36);
    CHECK(d.has("score") && !d.has("missing"));
    CHECK(d.get("missing", Value(vm, -1)).asInt() == -1);   // default
    // pairs() round-trip
    int seen = 0;
    for (const auto& [k, v] : d.pairs()) { (void)v; if (k.asStringRef() == "name" || k.asStringRef() == "age" || k.asStringRef() == "score") seen++; }
    CHECK(seen == 3);

    // --- Set builder ---
    Value s = Set(vm).add(1).add(2).add(2).add(3).build();
    CHECK(s.isSet() && s.len() == 3);

    // --- Args wrapper + round-trip through a registered native function authored with the API ---
    // A function that takes (a, b) Integers and a List, returning a Dict {sum, joined-list}.
    vm.registerGlobal("demo", vm.arena().alloc(std::make_unique<NativeFunction>(
        "demo", [](KiritoVM& kv, std::span<const Handle> raw) -> Handle {
            Args a(kv, raw, "demo");
            int64_t x = a.at(0).asInt("a");
            int64_t y = a.at(1).asInt("b");
            Value extra = a.opt(2, Value(kv, 0));
            List combined(kv);
            for (Value e : a[3].items()) combined.add(e);
            combined.add(extra);
            return Dict(kv).set("sum", x + y).set("items", combined.build()).build();
        })));

    CHECK(vm.stringify(vm.runSource("demo(2, 3, 99, [7, 8])[\"sum\"]")) == "5");
    CHECK(vm.stringify(vm.runSource("demo(2, 3, 99, [7, 8])[\"items\"]")) == "[7, 8, 99]");
    CHECK(vm.stringify(vm.runSource("demo(2, 3, 0, [])[\"items\"]")) == "[0]");
    // missing required arg -> clear error
    bool argErr = false;
    try { vm.runSource("demo(1)"); } catch (const KiritoError& e) {
        argErr = std::string(e.what()).find("missing argument") != std::string::npos;
    }
    CHECK(argErr);

    // --- GC safety: a builder roots its intermediates across allocations ---
    vm.setGcThreshold(1);   // collect aggressively to expose any unrooted intermediate
    List big(vm);
    for (int i = 0; i < 50; ++i) big.add(Value(vm, std::string("item") + std::to_string(i)));
    Value bigv = big.build();
    CHECK(bigv.len() == 50);
    CHECK(bigv.at(49).asStringRef() == "item49");   // survived GC during construction

    return RUN_TESTS();
}
