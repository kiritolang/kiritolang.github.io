// test_numeric_deep.cpp — adversarial/edge coverage for math/complex/matrix, filling audited gaps
// (concentrated in the ComplexMatrix error surface + non-integer index/argument type paths that the
// happy-path tests never provoke). Almost entirely throw-checks — the "try to break it" angle.
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

    // ---- non-integer index into a Matrix / ComplexMatrix throws (String/Float/Bool key) ----
    CHECK(throws(vm, "var matrix = import(\"matrix\")\nvar a = matrix.zeros(2, 2)\na[1.5, 0]"));
    CHECK(throws(vm, "var matrix = import(\"matrix\")\nvar a = matrix.zeros(2, 2)\na[\"x\"]"));
    CHECK(throws(vm, "var matrix = import(\"matrix\")\nvar a = matrix.zeros(2, 2)\na[True, 0]"));
    CHECK(throws(vm, "var complex = import(\"complex\")\nvar c = complex.zeros(2, 2)\nc[1.5, 0]"));
    CHECK(throws(vm, "var complex = import(\"complex\")\nvar c = complex.zeros(2, 2)\nc[\"x\", 0]"));

    // ---- ComplexMatrix resource cap (huge dims) throws, like Matrix's ----
    CHECK(throws(vm, "var complex = import(\"complex\")\ncomplex.zeros(100000, 100000)"));

    // ---- non-number cell in a Matrix / ComplexMatrix literal throws ----
    CHECK(throws(vm, "var matrix = import(\"matrix\")\nmatrix.Matrix([[\"a\"]])"));
    CHECK(throws(vm, "var complex = import(\"complex\")\ncomplex.Matrix([[\"a\"]])"));

    // ---- complex constructors/factories with a non-number arg throw ----
    CHECK(throws(vm, "var complex = import(\"complex\")\ncomplex.of(\"a\", 1)"));
    CHECK(throws(vm, "var complex = import(\"complex\")\ncomplex.Complex([])"));
    CHECK(throws(vm, "var complex = import(\"complex\")\ncomplex.real(\"x\")"));
    CHECK(throws(vm, "var complex = import(\"complex\")\ncomplex.polar(\"r\", 0)"));
    CHECK(throws(vm, "var complex = import(\"complex\")\ncomplex.vector([\"a\"])"));

    // ---- ComplexMatrix method out-of-range (get/set/row) throws ----
    CHECK(throws(vm, "var complex = import(\"complex\")\nvar c = complex.zeros(2, 2)\nc.get(5, 5)"));
    CHECK(throws(vm, "var complex = import(\"complex\")\nvar c = complex.zeros(2, 2)\nc.set(5, 5, complex.one)"));
    CHECK(throws(vm, "var complex = import(\"complex\")\nvar c = complex.zeros(2, 2)\nc.row(9)"));

    // ---- ComplexMatrix index-arity errors (3 keys on get, 1 key on set) throw ----
    CHECK(throws(vm, "var complex = import(\"complex\")\nvar c = complex.zeros(2, 2)\nc[0, 1, 2]"));
    CHECK(throws(vm, "var complex = import(\"complex\")\nvar c = complex.zeros(2, 2)\nc[0] = complex.one"));

    // ---- ComplexMatrix vector dot/cross with a non-ComplexMatrix arg throws ----
    CHECK(throws(vm, "var complex = import(\"complex\")\nvar v = complex.vector([1, 2, 3])\nv.dot(5)"));
    CHECK(throws(vm, "var complex = import(\"complex\")\nvar v = complex.vector([1, 2, 3])\nv.cross(5)"));

    // ---- ComplexMatrix unsupported operators (/, scalar-on-left) throw ----
    CHECK(throws(vm, "var complex = import(\"complex\")\nvar c = complex.zeros(2, 2)\nc / 2"));
    CHECK(throws(vm, "var complex = import(\"complex\")\nvar c = complex.zeros(2, 2)\n2 * c"));
    CHECK(throws(vm, "var complex = import(\"complex\")\nvar c = complex.zeros(2, 2)\n2.0 * c"));

    // ---- math.lcm / perm with a non-integer arg throw ("expects Integers") ----
    CHECK(throws(vm, "var math = import(\"math\")\nmath.lcm(1.5, 2)"));
    CHECK(throws(vm, "var math = import(\"math\")\nmath.perm(1.5, 2)"));

    // ---- positive sanity so the file also pins correct numeric behavior ----
    CHECK(run(vm, "var complex = import(\"complex\")\ncomplex.of(3, 4).modulus()") == "5.0");
    CHECK(run(vm, "var complex = import(\"complex\")\n(complex.i * complex.i).re") == "-1.0");
    CHECK(run(vm, "var matrix = import(\"matrix\")\nmatrix.identity(2).determinant()") == "1.0");
    CHECK(run(vm, "var math = import(\"math\")\nmath.lcm(4, 6)") == "12");

    return RUN_TESTS();
}
