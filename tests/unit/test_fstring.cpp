// f-string interpolation. The deterministic evalStr()-vs-hand-written-value cases (basic substitution,
// {{ }} escapes, expressions, format specs, all four flavours, raw-f, single-quote keys) live in
// tests/scripts/unit_fstring.ki. What remains HERE are the 3 placeholder-source-location checks: they
// assert on KiritoError::span (line/col of the REAL placeholder location, not the f-string token's
// start), a C++ embedding-API concept with no accessor exposed to Kirito scripts (a caught exception
// is just its message string), so they aren't expressible as a .ki test.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// "line:col" of the error a program throws — for asserting an f-string placeholder's TRUE location.
static std::string errLoc(const std::string& src) {
    KiritoVM vm;
    try { vm.runSource(src); return "OK"; }
    catch (const KiritoError& e) { return std::to_string(e.span.line) + ":" + std::to_string(e.span.col); }
}

int main() {
    // A01-2: a runtime error inside an f-string reports the placeholder's REAL source location, not the
    // f-string token's start (which only matched a LEADING placeholder). The reported column tracks the
    // `{`'s offset within the literal (prefix + quote + filler), and the LINE tracks the physical line
    // of the placeholder in a triple-quoted f-string.
    CHECK(errLoc("var d = {}\nf\"{d[1]}\"") == "2:5");                    // placeholder at the literal start
    CHECK(errLoc("var d = {}\nf\"xxxxxxxxxx{d[1]}\"") == "2:15");         // +10 filler -> column shifts by 10
    CHECK(errLoc("var d = {}\nvar s = f\"\"\"L1\nL2 {d[1]}\"\"\"") == "3:6");  // triple-quoted: real physical line + col

    return RUN_TESTS();
}
