#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // NOTE: math-module and pure path.join/gettempdir(-boolean) cases were converted to
    // tests/scripts/unit_stdlib.ki (TRIMMED here — see tests/unit/CLAUDE conversion notes).
    // The remaining cases below write real files under the OS temp directory and so stay in C++
    // per the golden-script determinism rule (no filesystem writes in checked-in .ki goldens).

    // --- file io: write then read back ---
    CHECK(evalStr(vm, R"(
var io = import("io")
var p = import("path").gettempdir() + "/kirito_stdlib_test.txt"
var f = io.open(p, "w")
f.write("hello\n")
f.write("world\n")
f.close()
var g = io.open(p, "r")
var content = g.read()
g.close()
content
)") == "hello\nworld\n");

    // --- file as a context manager (auto-close on exit) ---
    CHECK(evalStr(vm, R"(
var io = import("io")
var p = import("path").gettempdir() + "/kirito_stdlib_test2.txt"
with io.open(p, "w") as f:
    f.write("data 123")
var c = ""
with io.open(p, "r") as g:
    c = g.read()
c
)") == "data 123");

    // --- readline ---
    CHECK(evalStr(vm, R"(
var io = import("io")
var p = import("path").gettempdir() + "/kirito_stdlib_test3.txt"
var f = io.open(p, "w")
f.write("line1\nline2\n")
f.close()
var g = io.open(p, "r")
var first = g.readline()
g.close()
first
)") == "line1");

    return RUN_TESTS();
}
