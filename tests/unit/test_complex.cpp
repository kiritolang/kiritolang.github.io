#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

static const char* C = "var C = import(\"complex\")\n";

int main() {
    KiritoVM vm;
    auto run = [&](const std::string& body) { return evalStr(vm, std::string(C) + body); };

    // --- numbers: construction + string form (components are doubles) ---
    CHECK(run("C.of(3, 4)") == "3.0+4.0i");
    CHECK(run("C.of(1, -2)") == "1.0-2.0i");
    CHECK(run("C.real(5)") == "5.0+0.0i");
    CHECK(run("C.Complex(7)") == "7.0+0.0i");
    CHECK(run("C.i") == "0.0+1.0i");

    // --- arithmetic ---
    CHECK(run("C.of(1, 2) + C.of(3, 4)") == "4.0+6.0i");
    CHECK(run("C.of(5, 0) - C.of(2, 3)") == "3.0-3.0i");
    CHECK(run("C.of(1, 2) * C.of(3, 4)") == "-5.0+10.0i");   // (1+2i)(3+4i) = -5+10i
    CHECK(run("C.of(1, 1) / C.of(1, 1)") == "1.0+0.0i");
    CHECK(run("-C.of(2, -3)") == "-2.0+3.0i");
    CHECK(run("C.abs(C.of(1, 1) ** C.of(2, 0) - C.of(0, 2)) < 0.000000001") == "True");  // (1+i)^2 = 2i

    // --- equality (incl. Complex vs real) and accessors ---
    CHECK(run("C.of(1, 2) == C.of(1, 2)") == "True");
    CHECK(run("C.of(2, 0) == 2") == "True");
    CHECK(run("C.of(2, 1) == 2") == "False");
    CHECK(run("C.of(3, 4).re") == "3.0");
    CHECK(run("C.of(3, 4).im") == "4.0");

    // --- reductions ---
    CHECK(run("C.of(3, 4).conjugate()") == "3.0-4.0i");
    CHECK(run("C.of(3, 4).modulus()") == "5.0");
    CHECK(run("C.of(3, 4).norm2()") == "25.0");
    CHECK(run("C.zero.is_zero()") == "True");

    // --- analytic functions (modulus of the difference is ~0) ---
    // complex has no pi/e — use math's (single source of truth)
    CHECK(run("C.abs(C.exp(C.of(0, import(\"math\").pi)) - C.of(-1, 0)) < 0.000000001") == "True");  // Euler
    CHECK(run("C.sqrt(C.of(-1, 0))") == "0.0+1.0i");                                   // sqrt(-1) = i
    CHECK(run("C.abs(C.log(C.of(import(\"math\").e, 0)) - C.one) < 0.000000001") == "True");
    CHECK(run("C.polar(2.0, 0.0)") == "2.0+0.0i");

    // --- matrices: determinant (Gaussian) and inverse (Gauss-Jordan) ---
    const char* M = "var M = C.Matrix([[C.of(1,1), C.of(2,0)], [C.of(0,1), C.of(1,-1)]])\n";
    CHECK(evalStr(vm, std::string(C) + M + "M.rows()") == "2");
    CHECK(evalStr(vm, std::string(C) + M + "M[0, 0]") == "1.0+1.0i");
    CHECK(evalStr(vm, std::string(C) + M + "M.determinant()") == "2.0-2.0i");
    CHECK(evalStr(vm, std::string(C) + M + "M.trace()") == "2.0+0.0i");
    CHECK(evalStr(vm, std::string(C) + M + "M.transpose()[0, 1]") == "0.0+1.0i");
    CHECK(evalStr(vm, std::string(C) + M + "M.hermitian()[0, 1]") == "0.0-1.0i");
    // M * inverse(M) == I (via complex-matrix equality, within tolerance)
    CHECK(evalStr(vm, std::string(C) + M + "M * M.inverse() == C.identity(2)") == "True");

    CHECK(run("C.identity(3)") ==
          "[[1.0+0.0i, 0.0+0.0i, 0.0+0.0i], [0.0+0.0i, 1.0+0.0i, 0.0+0.0i], [0.0+0.0i, 0.0+0.0i, 1.0+0.0i]]");

    // --- complex vectors: Hermitian dot, v·v real, norm (`*` stays matrix multiply, not a dot) ---
    CHECK(run("C.vector([C.of(1,1), C.of(2,0)]).dot(C.vector([C.of(1,0), C.of(0,1)]))") == "1.0+1.0i");
    CHECK(run("C.vector([C.of(1,1), C.of(2,0)]).dot(C.vector([C.of(1,1), C.of(2,0)]))") == "6.0+0.0i");  // sum|z|^2
    CHECK(run("C.abs(C.of(C.vector([C.of(1,1), C.of(2,0)]).norm(), 0) - C.sqrt(C.of(6,0))) < 0.000000001") == "True");

    // --- error cases ---
    CHECK_THROWS(vm.runSource(std::string(C) + "C.one / C.zero\n"));                 // division by zero
    CHECK_THROWS(vm.runSource(std::string(C) + "C.one < C.of(2, 0)\n"));             // no ordering
    CHECK_THROWS(vm.runSource(std::string(C) + "C.Matrix([[C.of(1,0), C.of(2,0)], [C.of(2,0), C.of(4,0)]]).inverse()\n"));  // singular
    CHECK_THROWS(vm.runSource(std::string(C) + "C.Matrix([[C.of(1,0), C.of(2,0)], [C.of(3,0)]])\n"));  // ragged

    return RUN_TESTS();
}
