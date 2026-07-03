#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // var declaration and use
    CHECK(evalStr(vm, "var a = 10\na") == "10");
    // assignment rebinds
    CHECK(evalStr(vm, "var a = 1\na = a + 1\na") == "2");

    // Bool / None / String literals
    CHECK(evalStr(vm, "True") == "True");
    CHECK(evalStr(vm, "False") == "False");
    CHECK(evalStr(vm, "None") == "None");
    CHECK(evalStr(vm, "\"hi\"") == "hi");

    // string concatenation
    CHECK(evalStr(vm, "\"x\" + \"y\"") == "xy");

    // comparisons (Int/Float compare numerically; epsilon equality across types)
    CHECK(evalStr(vm, "1 == 1.0") == "True");
    CHECK(evalStr(vm, "2 < 3") == "True");
    CHECK(evalStr(vm, "3 <= 2") == "False");
    CHECK(evalStr(vm, "\"a\" < \"b\"") == "True");
    CHECK(evalStr(vm, "1 != 2") == "True");
    // equality across unrelated types is False, not an error
    CHECK(evalStr(vm, "1 == \"x\"") == "False");

    // undefined names are errors (read and write)
    CHECK_THROWS(vm.runSource("nope"));
    CHECK_THROWS(vm.runSource("nope = 1"));

    // reference assignment: `var b = a` binds b to the SAME value (same handle), no copy
    {
        Handle scope = vm.newModuleScope();
        vm.evalIn("var a = 5\nvar b = a\n", scope);  // compile + run in this scope
        auto& env = static_cast<EnvValue&>(vm.arena().deref(scope));
        const Handle* a = env.findLocal("a");
        const Handle* b = env.findLocal("b");
        CHECK(a != nullptr && b != nullptr);
        CHECK(*a == *b);
    }

    return RUN_TESTS();
}
