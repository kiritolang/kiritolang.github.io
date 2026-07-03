#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // Matrix supports m[i, j] read and write, and m[i] returns a row.
    const char* P = "var matrix = import(\"matrix\")\n"
                    "var m = matrix.Matrix([[1, 2, 3], [4, 5, 6]])\n";
    CHECK(evalStr(vm, std::string(P) + "m[0, 0]") == "1.0");
    CHECK(evalStr(vm, std::string(P) + "m[1, 2]") == "6.0");
    CHECK(evalStr(vm, std::string(P) + "m[1]") == "[4.0, 5.0, 6.0]");
    CHECK(evalStr(vm, std::string(P) + "m[0, 1] = 99\nm[0, 1]") == "99.0");
    CHECK_THROWS(vm.runSource(std::string(P) + "m[5, 5]\n"));
    CHECK_THROWS(vm.runSource(std::string(P) + "m[0]= 1\n"));  // assignment needs two indices

    // Single-index containers reject extra indices (arity is enforced).
    CHECK(evalStr(vm, "[10, 20, 30][1]") == "20");
    CHECK_THROWS(vm.runSource("[1, 2, 3][0, 1]\n"));
    CHECK_THROWS(vm.runSource("var d = {\"a\": 1}\nd[\"a\", \"b\"]\n"));
    CHECK_THROWS(vm.runSource("\"abc\"[0, 1]\n"));

    // Dict/List/String single indexing still works.
    CHECK(evalStr(vm, "{\"a\": 1, \"b\": 2}[\"b\"]") == "2");
    CHECK(evalStr(vm, "\"hello\"[1]") == "e");

    return RUN_TESTS();
}
