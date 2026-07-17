// test_strbytes_deep.cpp — adversarial/edge coverage for the String & Bytes types, filling audited
// gaps: type-check throws on the numeric secondary arguments (window start/end, split maxsplit,
// replace count, justify width), Bytes float-index / zero-step-slice / cross-type-ordering throws,
// UTF-8 validation branches (overlong, surrogate), and the encode/decode default + latin-1 paths.
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

    // ---- window helper: a Float start/end throws on every search method ----
    CHECK(throws(vm, "\"abcabc\".find(\"a\", 1.0)"));
    CHECK(throws(vm, "\"abcabc\".rfind(\"a\", 1.0)"));
    CHECK(throws(vm, "\"abcabc\".index(\"a\", 1.0)"));
    CHECK(throws(vm, "\"abcabc\".rindex(\"a\", 1.0)"));
    CHECK(throws(vm, "\"abcabc\".count(\"a\", 1.0)"));
    CHECK(throws(vm, "\"abcabc\".startswith(\"a\", 1.0)"));
    CHECK(throws(vm, "\"abcabc\".endswith(\"c\", 1.0)"));
    // but an Integer window works (sanity, and covers the [start, end] window itself)
    CHECK(run(vm, "\"abcabc\".find(\"a\", 1)") == "3");
    CHECK(run(vm, "\"abcabc\".count(\"a\", 0, 3)") == "1");
    CHECK(run(vm, "\"abcabc\".find(\"a\", 1, 2)") == "-1");

    // ---- split maxsplit / replace count as Float throw ----
    CHECK(throws(vm, "\"a,b,c\".split(\",\", 1.5)"));
    CHECK(throws(vm, "\"aaa\".replace(\"a\", \"b\", 1.5)"));
    // integer maxsplit / count still work
    CHECK(run(vm, "\"a,b,c\".split(\",\", 1)") == "['a', 'b,c']");
    CHECK(run(vm, "\"aaa\".replace(\"a\", \"b\", 2)") == "bba");

    // ---- justify/zfill Float width throws (neg + missing already covered elsewhere) ----
    CHECK(throws(vm, "\"x\".ljust(5.0)"));
    CHECK(throws(vm, "\"x\".rjust(5.0)"));
    CHECK(throws(vm, "\"x\".center(5.0)"));
    CHECK(throws(vm, "\"x\".zfill(5.0)"));

    // ---- Bytes float index + zero-step slice + cross-type ordering throw ----
    CHECK(throws(vm, "Bytes([1, 2, 3])[1.0]"));
    CHECK(throws(vm, "Bytes([1, 2, 3])[::0]"));
    CHECK(throws(vm, "Bytes([1]) < \"x\""));
    // a valid Bytes index / slice still works
    CHECK(run(vm, "Bytes([10, 20, 30])[1]") == "20");
    CHECK(run(vm, "Bytes([10, 20, 30])[-1]") == "30");

    // ---- UTF-8 validation: overlong encoding + surrogate range must throw on decode ----
    CHECK(throws(vm, "Bytes([0xc0, 0x80]).decode()"));        // overlong NUL
    CHECK(throws(vm, "Bytes([0xed, 0xa0, 0x80]).decode()"));  // UTF-16 surrogate D800
    CHECK(throws(vm, "Bytes([0xc3]).decode()"));              // truncated lead
    CHECK(throws(vm, "Bytes([0x80]).decode()"));              // stray continuation

    // ---- encode/decode: explicit None means the default (utf-8), same as omission ----
    CHECK(run(vm, "len(\"é\".encode(None))") == run(vm, "len(\"é\".encode())"));
    CHECK(run(vm, "\"é\".encode(None).decode(None)") == "é");
    CHECK(run(vm, "len(\"é\".encode())") == "2");             // é is 2 UTF-8 bytes

    // ---- latin-1 (iso-8859-1) is a lossless byte<->code-point map ----
    CHECK(run(vm, "\"é\".encode(\"iso-8859-1\")[0]") == "233");   // U+00E9 -> 0xE9
    CHECK(run(vm, "len(\"café\".encode(\"iso-8859-1\"))") == "4");
    CHECK(run(vm, "Bytes([233]).decode(\"latin-1\")") == "é");
    // ascii encoding of a non-ascii code point throws
    CHECK(throws(vm, "\"é\".encode(\"ascii\")"));

    // ---- round-trip: hex/fromhex, and Bytes ordering among Bytes ----
    CHECK(run(vm, "Bytes([255, 16, 0]).hex()") == "ff1000");
    CHECK(run(vm, "Bytes([1, 2]) < Bytes([1, 3])") == "True");
    CHECK(run(vm, "Bytes([1, 2]) < Bytes([1, 2, 0])") == "True");   // shorter prefix is smaller

    return RUN_TESTS();
}
