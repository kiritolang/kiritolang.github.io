#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // --- math module ---
    CHECK(evalStr(vm, "import(\"math\").sqrt(16)") == "4.0");
    CHECK(evalStr(vm, "var m = import(\"math\")\nm.sqrt(9)") == "3.0");
    CHECK(evalStr(vm, "import(\"math\").floor(3.7)") == "3");
    CHECK(evalStr(vm, "import(\"math\").ceil(3.2)") == "4");
    CHECK(evalStr(vm, "import(\"math\").factorial(5)") == "120");
    CHECK(evalStr(vm, "import(\"math\").gcd(12, 8)") == "4");
    CHECK(evalStr(vm, "import(\"math\").pow(2, 10)") == "1024.0");
    CHECK(evalStr(vm, "import(\"math\").log2(8)") == "3.0");
    CHECK(evalStr(vm, "var m = import(\"math\")\nm.pi > 3.14 and m.pi < 3.15") == "True");
    CHECK(evalStr(vm, "var m = import(\"math\")\nm.gcd(54, 24)") == "6");

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

    // --- path.gettempdir returns an existing directory (honors TMPDIR) ---
    CHECK(evalStr(vm, "var path = import(\"path\")\npath.isdir(path.gettempdir()) and len(path.gettempdir()) > 0") == "True");
    // --- path.join: os.path.join semantics (separator join; absolute part resets) ---
    CHECK(evalStr(vm, "import(\"path\").join(\"a\", \"b\", \"c\")") == "a/b/c");
    CHECK(evalStr(vm, "import(\"path\").join(\"/usr\", \"local\", \"bin\")") == "/usr/local/bin");
    CHECK(evalStr(vm, "import(\"path\").join(\"a\", \"/b\")") == "/b");

    return RUN_TESTS();
}
