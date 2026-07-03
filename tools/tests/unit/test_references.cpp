#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // --- self-referential containers ---
    // string conversion is cycle-guarded
    CHECK(evalStr(vm, "var x = []\nx.append(x)\nx") == "[[...]]");
    // iteration yields exactly the one element (itself)
    CHECK(evalStr(vm, "var x = []\nx.append(x)\nvar n = 0\nfor e in x:\n    n = n + 1\nn") == "1");
    // identity equality short-circuits (no infinite recursion)
    CHECK(evalStr(vm, "var x = []\nx.append(x)\nx == x") == "True");
    // two distinct cyclic structures: bounded, throws rather than crashing
    CHECK_THROWS(vm.runSource("var x = []\nx.append(x)\nvar y = []\ny.append(y)\nx == y\n"));

    // --- aliasing: a shared mutable element is visible through every slot ---
    CHECK(evalStr(vm, R"(
var a = [1]
var arr = [a, a, [9]]
a.append(2)
arr
)") == "[[1, 2], [1, 2], [9]]");
    CHECK(evalStr(vm, "var a = [1]\nvar arr = [a, a]\narr[0] == arr[1]") == "True");

    // --- reference assignment binds the same object (no copy) ---
    CHECK(evalStr(vm, "var a = [1]\nvar b = a\nb.append(2)\na") == "[1, 2]");

    // --- shallow copy: new container, shared elements ---
    CHECK(evalStr(vm, R"(
var a = [1]
var x = [a, a]
var y = x.copy()
a.append(9)
y
)") == "[[1, 9], [1, 9]]");        // copy shares element a
    CHECK(evalStr(vm, "var x = [1, 2]\nvar y = x.copy()\ny.append(3)\nlen(x)") == "2");  // independent top level
    CHECK(evalStr(vm, "var x = [1, 2]\nvar y = x.copy()\nx == y") == "True");

    CHECK(evalStr(vm, "var d = {\"a\": 1}\nvar e = d.copy()\ne[\"b\"] = 2\nlen(d)") == "1");

    // --- set algebra ---
    CHECK(evalStr(vm, "var u = {1, 2, 3}.union({3, 4})\nlen(u)") == "4");
    CHECK(evalStr(vm, "var i = {1, 2, 3}.intersection({2, 3, 4})\nlen(i)") == "2");
    CHECK(evalStr(vm, "var d = {1, 2, 3}.difference({2})\nlen(d)") == "2");
    CHECK(evalStr(vm, "var s = {1, 2, 3}\ns.remove(2)\nlen(s)") == "2");

    return RUN_TESTS();
}
