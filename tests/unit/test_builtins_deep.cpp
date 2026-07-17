// test_builtins_deep.cpp — adversarial/edge coverage for builtin functions, filling audited gaps:
// the format precision-too-large guard, the range double-keyword-for-one-slot clash, the x= keyword
// form of single-param signatured builtins, and format's `%` conversion combined with `,` grouping.
#include <string>
#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; }
    catch (...) { return true; }
}

int main() {
    KiritoVM vm;

    // ---- format precision beyond the resource cap throws (parallel to the width guard) ----
    CHECK(throws(vm, "format(1.0, \".999999999f\")"));
    // a sane precision still works
    CHECK(run(vm, "format(3.14159, \".2f\")") == "3.14");

    // ---- range: two keywords aliasing the same slot (stop + end) clash ----
    CHECK(throws(vm, "range(stop=5, end=3)"));
    // the lone end= alias still works
    CHECK(run(vm, "List(range(end=3))") == "[0, 1, 2]");

    // ---- x= keyword form of single-parameter signatured builtins ----
    CHECK(!throws(vm, "id(x=5)"));
    CHECK(!throws(vm, "inspect(x=5)"));

    // ---- format `%` conversion combined with `,` grouping on a value >= 1000 ----
    CHECK(run(vm, "format(12.5, \",.1%\")") == "1,250.0%");
    // and plain comma grouping of a large integer (sanity around the same branch)
    CHECK(run(vm, "format(1000000, \",\")") == "1,000,000");
    CHECK(run(vm, "format(255, \"#x\")") == "0xff");

    return RUN_TESTS();
}
