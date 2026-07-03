#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // define and call
    CHECK(evalStr(vm, R"(
var f = Function(x):
    return x + 1
f(10)
)") == "11");

    // recursion (the function sees its own name in the enclosing scope)
    CHECK(evalStr(vm, R"(
var fact = Function(n):
    if n <= 1:
        return 1
    return n * fact(n - 1)
fact(5)
)") == "120");

    // closure: inner function captures and rebinds an outer local across calls
    CHECK(evalStr(vm, R"(
var makeCounter = Function():
    var count = 0
    var inc = Function():
        count = count + 1
        return count
    return inc
var c = makeCounter()
c()
c()
c()
)") == "3");

    // higher-order: functions are first-class values
    CHECK(evalStr(vm, R"(
var apply = Function(g, v):
    return g(v)
var dbl = Function(x):
    return x * 2
apply(dbl, 21)
)") == "42");

    // a function with no return yields None
    CHECK(evalStr(vm, R"(
var f = Function():
    var x = 1
f()
)") == "None");

    // arity mismatch is a runtime error
    CHECK_THROWS(vm.runSource("var f = Function(x):\n    return x\nf(1, 2)\n"));
    CHECK_THROWS(vm.runSource("var f = Function(x):\n    return x\nf()\n"));

    // calling a non-callable is an error
    CHECK_THROWS(vm.runSource("var x = 5\nx()\n"));

    return RUN_TESTS();
}
