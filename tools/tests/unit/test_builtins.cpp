#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // range
    CHECK(evalStr(vm, "range(5)") == "[0, 1, 2, 3, 4]");
    CHECK(evalStr(vm, "range(2, 8, 2)") == "[2, 4, 6]");
    CHECK(evalStr(vm, "range(5, 0, -1)") == "[5, 4, 3, 2, 1]");
    CHECK(evalStr(vm, "var s = 0\nfor i in range(101):\n    s = s + i\ns") == "5050");

    // sum / min / max / abs / round
    CHECK(evalStr(vm, "sum([1, 2, 3, 4])") == "10");
    CHECK(evalStr(vm, "sum([1.5, 2.5])") == "4.0");
    CHECK(evalStr(vm, "min([5, 2, 8, 1])") == "1");
    CHECK(evalStr(vm, "max([5, 2, 8, 1])") == "8");
    CHECK(evalStr(vm, "min(3, 7, 1)") == "1");
    CHECK(evalStr(vm, "max(\"apple\", \"banana\")") == "banana");
    CHECK(evalStr(vm, "abs(-7)") == "7");
    CHECK(evalStr(vm, "abs(-3.5)") == "3.5");
    CHECK(evalStr(vm, "round(3.14159, 2)") == "3.14");
    CHECK(evalStr(vm, "round(2.7)") == "3");

    // sorted / enumerate / zip
    CHECK(evalStr(vm, "sorted([3, 1, 2])") == "[1, 2, 3]");
    CHECK(evalStr(vm, "sorted([\"c\", \"a\", \"b\"])") == "['a', 'b', 'c']");
    CHECK(evalStr(vm, "enumerate([\"a\", \"b\"])") == "[[0, 'a'], [1, 'b']]");
    CHECK(evalStr(vm, "zip([1, 2, 3], [4, 5])") == "[[1, 4], [2, 5]]");

    // inline anonymous functions (single-line body, explicit return)
    CHECK(evalStr(vm, "var sq = Function(x): return x * x\nsq(9)") == "81");
    CHECK(evalStr(vm, "map(Function(x): return x * x, [1, 2, 3])") == "[1, 4, 9]");
    CHECK(evalStr(vm, "filter(Function(x): return x % 2 == 0, range(6))") == "[0, 2, 4]");

    // type
    CHECK(evalStr(vm, "type(42)") == "Integer");
    CHECK(evalStr(vm, "type(3.5)") == "Float");
    CHECK(evalStr(vm, "type(\"hi\")") == "String");
    CHECK(evalStr(vm, "type([1])") == "List");
    CHECK(evalStr(vm, "type(True)") == "Bool");

    // expanded math
    CHECK(evalStr(vm, "var m = import(\"math\")\nm.cbrt(27)") == "3.0");
    CHECK(evalStr(vm, "import(\"math\").lcm(4, 6)") == "12");
    CHECK(evalStr(vm, "import(\"math\").trunc(3.9)") == "3.0");
    CHECK(evalStr(vm, "import(\"math\").isnan(0.0)") == "False");
    CHECK(evalStr(vm, "import(\"math\").copysign(3.0, -1.0)") == "-3.0");

    // --- bitwise integer builtins ---
    CHECK(evalStr(vm, "bitand(12, 10)") == "8");
    CHECK(evalStr(vm, "bitor(12, 10)") == "14");
    CHECK(evalStr(vm, "bitxor(12, 10)") == "6");
    CHECK(evalStr(vm, "bitnot(0)") == "-1");
    CHECK(evalStr(vm, "bitnot(5)") == "-6");
    CHECK(evalStr(vm, "shl(1, 8)") == "256");
    CHECK(evalStr(vm, "shr(256, 4)") == "16");
    CHECK(evalStr(vm, "shr(-8, 1)") == "-4");         // arithmetic (sign-preserving) right shift
    CHECK(evalStr(vm, "shl(1, 64)") == "0");          // shifting past the width is well-defined
    CHECK(evalStr(vm, "shr(1, 64)") == "0");
    CHECK(evalStr(vm, "shr(-1, 100)") == "-1");       // sign fill
    CHECK(evalStr(vm, "hex(bitor(15, 240))") == "0xff");
    CHECK(evalStr(vm, "bitand(a=12, b=10)") == "8");  // signatured -> keyword args
    CHECK_THROWS(vm.runSource("bitand(1.5, 2)"));     // non-Integer rejected
    CHECK_THROWS(vm.runSource("bitor(\"x\", 1)"));
    CHECK_THROWS(vm.runSource("shl(1, -2)"));         // negative shift rejected

    return RUN_TESTS();
}
