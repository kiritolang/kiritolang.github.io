#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

static const char* P = "var matrix = import(\"matrix\")\nvar A = matrix.Matrix([[1, 2], [3, 4]])\n"
                       "var B = matrix.Matrix([[5, 6], [7, 8]])\n";

int main() {
    KiritoVM vm;

    // construction and string form (elements are doubles)
    CHECK(evalStr(vm, std::string(P) + "A") == "[[1.0, 2.0], [3.0, 4.0]]");
    CHECK(evalStr(vm, std::string(P) + "A.shape()") == "[2, 2]");
    CHECK(evalStr(vm, std::string(P) + "A.rows()") == "2");

    // arithmetic
    CHECK(evalStr(vm, std::string(P) + "A + B") == "[[6.0, 8.0], [10.0, 12.0]]");
    CHECK(evalStr(vm, std::string(P) + "B - A") == "[[4.0, 4.0], [4.0, 4.0]]");
    CHECK(evalStr(vm, std::string(P) + "A * B") == "[[19.0, 22.0], [43.0, 50.0]]");
    CHECK(evalStr(vm, std::string(P) + "A * 2") == "[[2.0, 4.0], [6.0, 8.0]]");

    // transpose, determinant, trace
    CHECK(evalStr(vm, std::string(P) + "A.transpose()") == "[[1.0, 3.0], [2.0, 4.0]]");
    CHECK(evalStr(vm, std::string(P) + "A.determinant()") == "-2.0");
    CHECK(evalStr(vm, std::string(P) + "A.trace()") == "5.0");

    // inverse: A * inv(A) ~ I — computed floats, so use the tolerant .compare (Matrix == is exact)
    // abs_tol is needed because identity has exact zeros: comparing a near-zero computed off-diagonal
    // to 0.0 with only rel_tol fails (a relative tolerance against exactly 0.0 can never match).
    CHECK(evalStr(vm, std::string(P) + "(A * A.inverse()).compare(matrix.identity(2), abs_tol = 1e-9)") == "True");
    CHECK(evalStr(vm, "var m = import(\"matrix\")\nm.identity(3)") ==
          "[[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]");

    // a known 3x3 determinant
    CHECK(evalStr(vm, "import(\"matrix\").Matrix([[2, 0, 0], [0, 3, 0], [0, 0, 4]]).determinant()") == "24.0");

    // factories, apply, get/set
    CHECK(evalStr(vm, "import(\"matrix\").zeros(2, 3)") == "[[0.0, 0.0, 0.0], [0.0, 0.0, 0.0]]");
    CHECK(evalStr(vm, std::string(P) + "A.apply(Function(x): return x + 10)") ==
          "[[11.0, 12.0], [13.0, 14.0]]");
    CHECK(evalStr(vm, std::string(P) + "A.get(1, 0)") == "3.0");
    CHECK(evalStr(vm, std::string(P) + "A.set(0, 0, 99)\nA.get(0, 0)") == "99.0");
    CHECK(evalStr(vm, std::string(P) + "A.row(1)") == "[3.0, 4.0]");
    CHECK(evalStr(vm, std::string(P) + "A.sum()") == "10.0");

    // vectors (a Matrix with one dimension == 1): dot, cross, norm. The dot product is `u.dot(v)`;
    // `*` stays matrix multiply, so two same-shape (row) vectors are an inner-dimension error.
    CHECK(evalStr(vm, "var m = import(\"matrix\")\nm.vector([1, 2, 3]).dot(m.vector([4, 5, 6]))") == "32.0");
    CHECK(evalStr(vm, "var m = import(\"matrix\")\nm.vector([1, 0, 0]).cross(m.vector([0, 1, 0]))") == "[[0.0, 0.0, 1.0]]");
    CHECK(evalStr(vm, "var m = import(\"matrix\")\nm.vector([3, 4]).norm()") == "5.0");
    CHECK_THROWS(vm.runSource("var m = import(\"matrix\")\nm.vector([1, 2]).dot(m.vector([1, 2, 3]))\n"));   // length mismatch
    CHECK_THROWS(vm.runSource("var m = import(\"matrix\")\nm.vector([1, 2]).cross(m.vector([3, 4]))\n"));     // not 3-element
    CHECK_THROWS(vm.runSource("var m = import(\"matrix\")\nm.vector([1, 2, 3]) * m.vector([4, 5, 6])\n"));    // * is matrix multiply only

    // error cases
    CHECK_THROWS(vm.runSource(std::string(P) + "matrix.Matrix([[1, 2], [3, 4], [5, 6]]).inverse()\n"));  // non-square
    CHECK_THROWS(vm.runSource("import(\"matrix\").Matrix([[1, 2]]) + import(\"matrix\").Matrix([[1], [2]])\n"));  // shape
    CHECK_THROWS(vm.runSource("import(\"matrix\").Matrix([[1, 1], [1, 1]]).inverse()\n"));  // singular
    CHECK_THROWS(vm.runSource("import(\"matrix\").Matrix([[1, 2], [3]])\n"));  // ragged

    return RUN_TESTS();
}
