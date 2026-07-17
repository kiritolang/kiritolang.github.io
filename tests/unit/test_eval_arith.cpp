#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static int64_t evalInt(KiritoVM& vm, const std::string& src) {
    Handle h = vm.runSource(src);
    const Object& o = vm.arena().deref(h);
    CHECK(o.kind() == ValueKind::Integer);
    return static_cast<const IntVal&>(o).value();
}

static double evalFloat(KiritoVM& vm, const std::string& src) {
    Handle h = vm.runSource(src);
    const Object& o = vm.arena().deref(h);
    CHECK(o.kind() == ValueKind::Float);
    return static_cast<const FloatVal&>(o).value();
}

int main() {
    KiritoVM vm;

    // precedence and associativity
    CHECK(evalInt(vm, "1 + 2 * 3") == 7);
    CHECK(evalInt(vm, "(1 + 2) * 3") == 9);
    CHECK(evalInt(vm, "2 ** 3 ** 2") == 512);  // right-associative
    CHECK(evalInt(vm, "-2 ** 2") == -4);       // ** binds tighter than unary minus

    // floor division and modulo (sign follows divisor)
    CHECK(evalInt(vm, "7 // 2") == 3);
    CHECK(evalInt(vm, "-7 // 2") == -4);
    CHECK(evalInt(vm, "7 % 3") == 1);
    CHECK(evalInt(vm, "-7 % 3") == 2);

    // true division always yields Float
    CHECK(evalFloat(vm, "4 / 2") == 2.0);
    CHECK(evalFloat(vm, "1 / 2") == 0.5);

    // Float arithmetic and Int/Float promotion
    CHECK(evalFloat(vm, "1.5 + 2.5") == 4.0);
    CHECK(evalFloat(vm, "2 * 1.5") == 3.0);

    // negative exponent promotes to Float
    CHECK(evalFloat(vm, "2 ** -1") == 0.5);

    // stringify: Float keeps a decimal point
    CHECK(vm.stringify(vm.runSource("4 / 2")) == "2.0");
    CHECK(vm.stringify(vm.runSource("3 + 4")) == "7");

    // division by zero is a runtime error
    CHECK_THROWS(vm.runSource("1 / 0"));
    CHECK_THROWS(vm.runSource("5 // 0"));
    CHECK_THROWS(vm.runSource("5 % 0"));

    // base-prefixed integer literals: hex (0x), binary (0b), octal (0o), case-insensitive
    CHECK(evalInt(vm, "0xFF") == 255);
    CHECK(evalInt(vm, "0xff") == 255);
    CHECK(evalInt(vm, "0Xf0") == 240);
    CHECK(evalInt(vm, "0b1010") == 10);
    CHECK(evalInt(vm, "0B1100") == 12);
    CHECK(evalInt(vm, "0o17") == 15);
    CHECK(evalInt(vm, "0xFFFFFFFFFFFFFFFF") == -1);  // full-width two's-complement wraparound
    CHECK(evalInt(vm, "0x10 + 0b1") == 17);          // usable in expressions
    CHECK(evalInt(vm, "[0xA, 0xB][1]") == 11);       // and as call/collection args
    // a prefix with no digits, or a stray base, is a lex/parse error (not a silent 0 + identifier)
    CHECK_THROWS(vm.runSource("var x = 0x\n"));
    CHECK_THROWS(vm.runSource("var x = 0b2\n"));     // 2 isn't a binary digit -> trailing token

    return RUN_TESTS();
}
