// Coverage for the Value/Args facade methods (value.hpp) that test_value.cpp doesn't exercise:
// kind/typeName/truthy/str, items() over non-list iterables + the not-iterable throw, the len()/at()
// /asX/has/get error paths, Args::empty/size, get-with-default, and the std::vector makeList overload.
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    KiritoVM vm;

    // ---- kind / typeName / truthy / str ----
    CHECK(Value(vm, 1).kind() == ValueKind::Integer);
    CHECK(Value(vm, 1.0).typeName() == "Float");
    CHECK(Value(vm, std::string("x")).typeName() == "String");
    CHECK(Value(vm, 0).truthy() == false);
    CHECK(Value(vm, 5).truthy() == true);
    CHECK(Value::None(vm).truthy() == false);
    CHECK(Value(vm, std::string("")).truthy() == false);
    CHECK(List(vm).add(1).add(2).build().str() == "[1, 2]");
    CHECK(Value(vm, 42).str() == "42");

    // ---- items() over every iterable kind ----
    CHECK(List(vm).add(1).add(2).add(3).build().items().size() == 3);
    {
        std::vector<Value> chars = Value(vm, std::string("aéz")).items();  // String -> code points
        CHECK(chars.size() == 3 && chars[1].asStringRef() == "é");
    }
    CHECK(Set(vm).add(1).add(2).build().items().size() == 2);            // Set -> elements
    {
        std::vector<Value> keys = Dict(vm).set("a", Value(vm, 1)).set("b", Value(vm, 2)).build().items();
        CHECK(keys.size() == 2);                                          // Dict -> keys
    }
    CHECK_THROWS(Value(vm, 5).items());                                     // Integer is not iterable

    // ---- pairs() over a Dict ----
    {
        auto ps = Dict(vm).set("k", Value(vm, 9)).build().pairs();
        CHECK(ps.size() == 1 && ps[0].first.asStringRef() == "k" && ps[0].second.asInt() == 9);
    }

    // ---- at() on a list, negative + out of range ----
    {
        Value xs = List(vm).add(10).add(20).add(30).build();
        CHECK(xs.at(0).asInt() == 10);
        CHECK(xs.at(-1).asInt() == 30);          // negatives count from the end
        CHECK_THROWS(xs.at(99));                  // out of range throws
    }

    // ---- len() success + error path ----
    CHECK(List(vm).add(1).add(2).build().len() == 2);
    CHECK(Value(vm, std::string("héllo")).len() == 5);
    CHECK_THROWS(Value(vm, 5).len());               // Integer has no length

    // ---- typed-read mismatches all throw ----
    CHECK_THROWS(Value(vm, std::string("x")).asInt());
    CHECK_THROWS(Value(vm, 1).asStringRef());
    CHECK_THROWS(Value(vm, std::string("x")).asBool());
    CHECK_THROWS(Value(vm, std::string("x")).asFloat());
    CHECK(Value(vm, 7).asFloat() == 7.0);           // asFloat accepts an Integer

    // ---- has / get on a Dict + error on a non-Dict ----
    {
        Value d = Dict(vm).set("name", Value(vm, std::string("Ada"))).build();
        CHECK(d.has("name") == true && d.has("missing") == false);
        CHECK(d.get("name").asStringRef() == "Ada");
        CHECK(d.get("missing", Value(vm, 0)).asInt() == 0);   // default for an absent key
        CHECK_THROWS(d.get("missing"));                      // no default -> throws
        CHECK_THROWS(Value(vm, 1).has("k"));                   // not a Dict
    }

    // ---- makeList from a std::vector<Handle> ----
    {
        std::vector<Handle> hs{vm.makeInt(1), vm.makeInt(2), vm.makeInt(3)};
        CHECK(List(vm, hs).len() == 3);
    }

    // ---- Args: empty / size / at / opt, via a registered native ----
    vm.registerGlobal("probe", vm.arena().alloc(std::make_unique<NativeFunction>(
        "probe", [](KiritoVM& kv, std::span<const Handle> raw) -> Handle {
            Args a(kv, raw, "probe");
            // returns "size=N empty=B first=X opt=Y"
            std::string out = "size=" + std::to_string(a.size()) +
                              " empty=" + (a.empty() ? "1" : "0");
            if (!a.empty()) out += " first=" + std::to_string(a.at(0).asInt("first"));
            out += " opt=" + std::to_string(a.opt(1, val(kv, 99)).asInt("opt"));
            return kv.makeString(out);
        })));
    CHECK(vm.stringify(vm.runSource("probe()")) == "size=0 empty=1 opt=99");
    CHECK(vm.stringify(vm.runSource("probe(7)")) == "size=1 empty=0 first=7 opt=99");
    CHECK(vm.stringify(vm.runSource("probe(7, 8)")) == "size=2 empty=0 first=7 opt=8");

    return RUN_TESTS();
}
