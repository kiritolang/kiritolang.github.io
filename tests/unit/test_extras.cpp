#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // pass
    CHECK(evalStr(vm, "if True:\n    pass\n\"ok\"") == "ok");
    CHECK(evalStr(vm, "var f = Function():\n    pass\nf()") == "None");

    // assert
    CHECK(evalStr(vm, "assert 2 + 2 == 4\n\"passed\"") == "passed");
    CHECK_THROWS(vm.runSource("assert 1 == 2\n"));
    CHECK(evalStr(vm, R"(
try:
    assert False, "custom message"
catch as e:
    e
)") == "custom message");

    // list methods
    CHECK(evalStr(vm, "var a = [3, 1, 2]\na.sort()\na") == "[1, 2, 3]");
    CHECK(evalStr(vm, "var a = [\"banana\", \"apple\", \"cherry\"]\na.sort()\na") ==
          "['apple', 'banana', 'cherry']");
    CHECK(evalStr(vm, "var a = [1, 2, 3]\na.reverse()\na") == "[3, 2, 1]");
    CHECK(evalStr(vm, "var a = [1, 2, 4]\na.insert(2, 3)\na") == "[1, 2, 3, 4]");
    CHECK(evalStr(vm, "var a = [1, 2, 3]\na.remove(2)\na") == "[1, 3]");
    CHECK(evalStr(vm, "[10, 20, 30].index(20)") == "1");
    CHECK(evalStr(vm, "var a = [1, 2]\na.extend([3, 4])\na") == "[1, 2, 3, 4]");

    // dict methods
    CHECK(evalStr(vm, "var d = {\"a\": 1, \"b\": 2}\nd.get(\"a\")") == "1");
    CHECK(evalStr(vm, "{\"a\": 1}.get(\"z\", 99)") == "99");
    CHECK(evalStr(vm, "{\"a\": 1}.get(\"z\")") == "None");
    CHECK(evalStr(vm, "var d = {1: 1, 2: 2, 3: 3}\nvar ks = d.keys()\nks.sort()\nks") == "[1, 2, 3]");
    CHECK(evalStr(vm, "len({1: 1, 2: 2, 3: 3}.values())") == "3");
    CHECK(evalStr(vm, "len({1: 1, 2: 2}.items())") == "2");
    CHECK(evalStr(vm, "var d = {\"x\": 5}\nd.pop(\"x\")") == "5");
    CHECK(evalStr(vm, "var d = {\"x\": 5, \"y\": 6}\nd.pop(\"x\")\nlen(d)") == "1");

    return RUN_TESTS();
}
