#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // --- List ---
    CHECK(evalStr(vm, "[1, 2, 3]") == "[1, 2, 3]");
    CHECK(evalStr(vm, "len([1, 2, 3])") == "3");
    CHECK(evalStr(vm, "var a = [1, 2, 3]\na[0] + a[2]") == "4");
    CHECK(evalStr(vm, "var a = [1, 2, 3]\na[-1]") == "3");
    CHECK(evalStr(vm, "var a = [1, 2, 3]\na[0] = 10\na[0]") == "10");
    CHECK(evalStr(vm, R"(
var a = [1, 2, 3, 4]
var s = 0
for x in a:
    s = s + x
s
)") == "10");
    CHECK(evalStr(vm, R"(
var a = [1]
a.append(2)
a.append(3)
len(a)
)") == "3");
    CHECK_THROWS(vm.runSource("[1, 2][5]"));

    // --- Dict ---
    CHECK(evalStr(vm, "var d = {\"a\": 1, \"b\": 2}\nd[\"a\"] + d[\"b\"]") == "3");
    CHECK(evalStr(vm, "var d = {}\nd[\"x\"] = 5\nd[\"x\"]") == "5");
    CHECK(evalStr(vm, "len({\"a\": 1, \"b\": 2})") == "2");
    CHECK(evalStr(vm, R"(
var d = {1: 10, 2: 20}
var s = 0
for k in d:
    s = s + k
s
)") == "3");
    CHECK_THROWS(vm.runSource("{\"a\": 1}[\"z\"]"));
    CHECK_THROWS(vm.runSource("var d = {}\nd[[1]] = 1"));  // unhashable key

    // --- Set ---
    CHECK(evalStr(vm, "len({1, 2, 2, 3})") == "3");
    CHECK(evalStr(vm, "{1, 2, 3}.contains(2)") == "True");
    CHECK(evalStr(vm, "{1, 2, 3}.contains(9)") == "False");
    CHECK(evalStr(vm, R"(
var s = {1}
s.add(2)
s.add(2)
len(s)
)") == "2");

    // --- equality (structural, nested) ---
    CHECK(evalStr(vm, "[1, 2] == [1, 2]") == "True");
    CHECK(evalStr(vm, "[1, 2] == [1, 3]") == "False");
    CHECK(evalStr(vm, "[[1, 2], [3]] == [[1, 2], [3]]") == "True");
    CHECK(evalStr(vm, "{1: 2} == {1: 2}") == "True");

    // --- cycle-safe stringify ---
    CHECK(evalStr(vm, "var a = []\na.append(a)\na") == "[[...]]");

    return RUN_TESTS();
}
