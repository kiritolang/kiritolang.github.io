#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // while loop
    CHECK(evalStr(vm, R"(
var a = 0
while a < 5:
    a = a + 1
a
)") == "5");

    // if / elif / else picks the right branch
    CHECK(evalStr(vm, R"(
var x = 2
var r = 0
if x == 1:
    r = 10
elif x == 2:
    r = 20
else:
    r = 30
r
)") == "20");

    // break exits the loop early
    CHECK(evalStr(vm, R"(
var i = 0
var s = 0
while True:
    i = i + 1
    if i > 3:
        break
    s = s + i
s
)") == "6");

    // continue skips the rest of the iteration
    CHECK(evalStr(vm, R"(
var i = 0
var s = 0
while i < 5:
    i = i + 1
    if i == 3:
        continue
    s = s + i
s
)") == "12");

    // and/or short-circuit: the right operand is not evaluated, and the deciding operand is returned
    CHECK(evalStr(vm, "True or 1 // 0") == "True");
    CHECK(evalStr(vm, "False and 1 // 0") == "False");
    CHECK(evalStr(vm, "1 and 2") == "2");
    CHECK(evalStr(vm, "0 or 3") == "3");

    // not
    CHECK(evalStr(vm, "not 0") == "True");
    CHECK(evalStr(vm, "not 1") == "False");
    CHECK(evalStr(vm, "not \"\"") == "True");

    // conditional expression: `then if cond else orelse`
    CHECK(evalStr(vm, "\"yes\" if 1 < 2 else \"no\"") == "yes");
    CHECK(evalStr(vm, "\"yes\" if 1 > 2 else \"no\"") == "no");
    CHECK(evalStr(vm, "(10 if True else 20) + 1") == "11");        // usable as a sub-expression
    // right-associative: a if c1 else b if c2 else c  ==  a if c1 else (b if c2 else c)
    CHECK(evalStr(vm, "\"A\" if False else \"B\" if True else \"C\"") == "B");
    CHECK(evalStr(vm, "\"A\" if False else \"B\" if False else \"C\"") == "C");
    // short-circuit: only the chosen branch is evaluated (the other may reference an undefined name)
    CHECK(evalStr(vm, "0 if True else 1 // 0") == "0");
    CHECK(evalStr(vm, "1 // 0 if False else 0") == "0");
    // nests inside lists, calls, and lower-precedence contexts
    CHECK(evalStr(vm, "[1 if True else 0, 2 if False else 9]") == "[1, 9]");
    CHECK(evalStr(vm, "abs(-3 if 1 > 0 else 3)") == "3");
    // binds looser than `or`: `a or b if c else d` parses as `(a or b) if c else d`
    CHECK(evalStr(vm, "False or True if False else 7") == "7");
    // a missing `else` is a clean parse error, not a crash
    CHECK_THROWS(vm.runSource("1 if True"));

    return RUN_TESTS();
}
