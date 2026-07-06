// Binary I/O through sys.createprocess / sys.shell: a Bytes `input=` reaches the child VERBATIM and
// `binary=True` returns raw Bytes stdout/stderr. This drives REAL POSIX subprocesses (cat / wc / od),
// so it is a Linux-only gate (the C++ suite is built + run on Linux); the cross-platform behaviour is
// pinned in spec_proc_binary.ki via the byte-transparent `ki` echo helper.
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string ok(const std::string& src) {
    KiritoVM vm;
    vm.installStandardLibrary();
    return vm.stringify(vm.runSource(src));
}
static std::string err(const std::string& src) {
    KiritoVM vm;
    vm.installStandardLibrary();
    try { vm.runSource(src); return ""; }
    catch (const std::exception& e) { return e.what(); }
}
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
// A `Bytes([...])` literal listing the given byte values.
static std::string bytesLiteral(const std::vector<int>& v) {
    std::string s = "Bytes([";
    for (std::size_t i = 0; i < v.size(); ++i) { if (i) s += ", "; s += std::to_string(v[i]); }
    s += "])";
    return s;
}

int main() {
    const std::string P = "var sys = import(\"sys\")\n";

    // --- binary=True returns Bytes; default returns String -------------------------------------
    CHECK(ok(P + "type(sys.shell(\"cat\", input = Bytes([1, 2, 3]), binary = True)[\"stdout\"])") == "Bytes");
    CHECK(ok(P + "type(sys.shell(\"cat\", input = Bytes([1, 2, 3]))[\"stdout\"])") == "String");

    // --- a Bytes input reaches the child VERBATIM: `wc -c` (a foreign byte counter) --------------
    // 0xFF is ONE byte; the pre-fix String path would have UTF-8-ballooned it to two.
    CHECK(ok(P + "Integer(sys.shell(\"wc -c\", input = Bytes([255]))[\"stdout\"].strip())") == "1");
    CHECK(ok(P + "Integer(sys.shell(\"wc -c\", input = Bytes([255, 254, 253, 128, 0]))[\"stdout\"].strip())") == "5");
    CHECK(ok(P + "Integer(sys.shell(\"wc -c\", input = Bytes([]))[\"stdout\"].strip())") == "0");
    // `od` dumps the exact bytes the child read
    CHECK(ok(P + "sys.shell(\"od -An -tx1\", input = Bytes([255, 128, 195, 40]), "
                 "binary = True)[\"stdout\"].decode().split() == [\"ff\", \"80\", \"c3\", \"28\"]") == "True");

    // --- exact round-trip through `cat` (a pure byte pipe) --------------------------------------
    CHECK(ok(P + "var b = Bytes([255, 0, 128, 195, 40, 10, 13, 26, 254])\n"
                 "sys.shell(\"cat\", input = b, binary = True)[\"stdout\"] == b") == "True");
    // all 256 byte values
    CHECK(ok(P + "var xs = []\nvar i = 0\nwhile i < 256:\n    xs.append(i)\n    i = i + 1\n"
                 "var b = Bytes(xs)\nsys.shell(\"cat\", input = b, binary = True)[\"stdout\"] == b") == "True");
    // empty
    CHECK(ok(P + "sys.shell(\"cat\", input = Bytes([]), binary = True)[\"stdout\"] == Bytes([])") == "True");

    // --- createprocess (argv, no shell) also honours Bytes input + binary out -------------------
    CHECK(ok(P + "var b = Bytes([0, 255, 254, 1])\n"
                 "sys.createprocess([\"cat\"], input = b, binary = True)[\"stdout\"] == b") == "True");

    // --- String input is fed as its UTF-8 encoding; binary output returns those bytes -----------
    CHECK(ok(P + "sys.shell(\"cat\", input = \"AB\", binary = True)[\"stdout\"] == Bytes([65, 66])") == "True");
    CHECK(ok(P + "sys.shell(\"cat\", input = chr(233), binary = True)[\"stdout\"] == Bytes([195, 169])") == "True");  // é -> UTF-8 0xC3 0xA9

    // --- binary stderr + exit code coexist ------------------------------------------------------
    CHECK(ok(P + "var r = sys.shell(\"cat 1>&2\", input = Bytes([255, 200, 0]), binary = True)\n"
                 "r[\"stderr\"] == Bytes([255, 200, 0]) and r[\"stdout\"] == Bytes([])") == "True");
    CHECK(ok(P + "sys.shell(\"cat >/dev/null; exit 5\", input = Bytes([9, 9]), binary = True)[\"code\"]") == "5");

    // --- adversarial: wrong `input` types are rejected before spawning --------------------------
    CHECK(has(err(P + "sys.shell(\"cat\", input = 123)"), "String or Bytes"));
    CHECK(has(err(P + "sys.shell(\"cat\", input = [1, 2, 3])"), "String or Bytes"));
    CHECK(has(err(P + "sys.createprocess([\"cat\"], input = {\"a\": 1})"), "String or Bytes"));

    // --- a large binary payload (200 KB) survives byte-exact ------------------------------------
    CHECK(ok(P + "var b = Bytes([137, 80, 78, 71, 255, 0]) * 40000\n"      // 240 000 bytes
                 "var out = sys.shell(\"cat\", input = b, binary = True)[\"stdout\"]\n"
                 "out == b and len(out) == 240000") == "True");

    // --- FUZZ: random-length, random-value Bytes round-trip through `cat`, byte-exact -----------
    // C++-side generator (deterministic seed) so this contributes independent random coverage.
    {
        std::mt19937 rng(0xC0FFEEu);
        std::uniform_int_distribution<int> lenD(0, 300), byteD(0, 255);
        int checked = 0;
        for (int it = 0; it < 30; ++it) {
            std::vector<int> v(static_cast<std::size_t>(lenD(rng)));
            for (int& x : v) x = byteD(rng);
            const std::string lit = bytesLiteral(v);
            // round-trip equality AND exact length (a ballooned send would inflate the length)
            const std::string src = P + "var b = " + lit + "\n"
                "var out = sys.shell(\"cat\", input = b, binary = True)[\"stdout\"]\n"
                "out == b and len(out) == " + std::to_string(v.size());
            CHECK(ok(src) == "True");
            ++checked;
        }
        CHECK(checked == 30);
    }

    return RUN_TESTS();
}
