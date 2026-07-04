// test_kimods_deep.cpp — adversarial/edge coverage for the Kirito-authored frozen modules, filling
// audited gaps: copy.copy/deepcopy of native VALUE objects (the _copyViaSerde branch) and of an
// unserializable resource (best-effort fallback), statistics.mode on Float data, itertools.starmap
// with non-binary arity, accumulate's func= keyword, csv CRLF parsing, and enum definition-order.
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

    // ---- copy.deepcopy / copy.copy of a native VALUE object (the _copyViaSerde path) ----
    CHECK(run(vm, "var copy = import(\"copy\")\nvar matrix = import(\"matrix\")\nvar m = matrix.identity(2)\ncopy.deepcopy(m) == m") == "True");
    CHECK(run(vm, "var copy = import(\"copy\")\nvar matrix = import(\"matrix\")\nvar m = matrix.identity(3)\ncopy.copy(m) == m") == "True");
    CHECK(run(vm, "var copy = import(\"copy\")\nvar time = import(\"time\")\nvar d = time.datetime(1000)\ncopy.deepcopy(d) == d") == "True");
    CHECK(run(vm, "var copy = import(\"copy\")\nvar complex = import(\"complex\")\nvar z = complex.of(3, 4)\ncopy.deepcopy(z) == z") == "True");
    CHECK(run(vm, "var copy = import(\"copy\")\nvar tensor = import(\"tensor\")\nvar t = tensor.tensor([1, 2, 3])\ncopy.deepcopy(t) == t") == "True");
    // a deep copy is independent: mutating the source does not touch the copy
    CHECK(run(vm, R"KI(var copy = import("copy")
var a = [[1, 2], [3, 4]]
var b = copy.deepcopy(a)
a[0][0] = 99
b[0][0])KI") == "1");

    // ---- copy of an UNSERIALIZABLE resource returns best-effort (must not throw) ----
    CHECK(!throws(vm, "var copy = import(\"copy\")\nvar regex = import(\"regex\")\ndiscard copy.copy(regex.compile(\"a\"))"));
    CHECK(!throws(vm, "var copy = import(\"copy\")\nvar regex = import(\"regex\")\ndiscard copy.deepcopy(regex.compile(\"a\"))"));

    // ---- statistics.mode on Float data ----
    CHECK(run(vm, "var statistics = import(\"statistics\")\nstatistics.mode([1.5, 2.5, 1.5])") == "1.5");

    // ---- itertools.starmap with a non-binary (3-arg) function ----
    CHECK(run(vm, R"KI(var itertools = import("itertools")
List(itertools.starmap(Function(t): return t[0] + t[1] + t[2], [[1, 2, 3], [4, 5, 6]])))KI") == "[6, 15]");

    // ---- itertools.accumulate with the func= keyword (running product) ----
    CHECK(run(vm, R"KI(var itertools = import("itertools")
List(itertools.accumulate([1, 2, 3, 4], func=Function(a, b): return a * b)))KI") == "[1, 2, 6, 24]");

    // ---- csv.parse handles CRLF line endings ----
    CHECK(run(vm, R"KI(var csv = import("csv")
csv.parse("a,b\r\nc,d"))KI") == "[['a', 'b'], ['c', 'd']]");

    // ---- enum.values() returns ordinals in DEFINITION order, not hash order ----
    CHECK(run(vm, "var enum = import(\"enum\")\nenum.Enum([\"B\", \"A\"]).values()") == "[0, 1]");
    CHECK(run(vm, "var enum = import(\"enum\")\nenum.Enum([\"B\", \"A\"]).names()") == "['B', 'A']");
    CHECK(run(vm, "var enum = import(\"enum\")\nenum.Enum([\"B\", \"A\"]).get(\"A\")") == "1");

    return RUN_TESTS();
}
