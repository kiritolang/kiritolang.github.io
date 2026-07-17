#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

// "line:col" of the error a program throws — for asserting an f-string placeholder's TRUE location.
static std::string errLoc(const std::string& src) {
    KiritoVM vm;
    try { vm.runSource(src); return "OK"; }
    catch (const KiritoError& e) { return std::to_string(e.span.line) + ":" + std::to_string(e.span.col); }
}

int main() {
    KiritoVM vm;

    // A01-2: a runtime error inside an f-string reports the placeholder's REAL source location, not the
    // f-string token's start (which only matched a LEADING placeholder). The reported column tracks the
    // `{`'s offset within the literal (prefix + quote + filler), and the LINE tracks the physical line
    // of the placeholder in a triple-quoted f-string.
    CHECK(errLoc("var d = {}\nf\"{d[1]}\"") == "2:5");                    // placeholder at the literal start
    CHECK(errLoc("var d = {}\nf\"xxxxxxxxxx{d[1]}\"") == "2:15");         // +10 filler -> column shifts by 10
    CHECK(errLoc("var d = {}\nvar s = f\"\"\"L1\nL2 {d[1]}\"\"\"") == "3:6");  // triple-quoted: real physical line + col

    CHECK(evalStr(vm, "var n = 7\nf\"n is {n}\"") == "n is 7");
    CHECK(evalStr(vm, "var a = 3\nvar b = 4\nf\"{a} + {b} = {a + b}\"") == "3 + 4 = 7");
    CHECK(evalStr(vm, "f\"{{escaped}} {1 + 1}\"") == "{escaped} 2");
    CHECK(evalStr(vm, "var name = \"World\"\nf\"Hello, {name}!\"") == "Hello, World!");
    CHECK(evalStr(vm, "f\"{[1, 2, 3]}\"") == "[1, 2, 3]");
    CHECK(evalStr(vm, "f\"no placeholders\"") == "no placeholders");
    CHECK(evalStr(vm, "var x = 10\nf\"{x // 3} remainder {x % 3}\"") == "3 remainder 1");
    CHECK(evalStr(vm, "var p = [5, 9]\nf\"sum is {p[0] + p[1]}\"") == "sum is 14");

    // format specs (f"{x:05d}") — a `:` after the expression applies a mini-format-spec
    CHECK(evalStr(vm, "var x = 42\nf\"{x:05d}\"") == "00042");
    CHECK(evalStr(vm, "var pi = 3.14159\nf\"{pi:.2f}\"") == "3.14");
    CHECK(evalStr(vm, "var x = 42\nf\"{x:#x}\"") == "0x2a");
    CHECK(evalStr(vm, "var x = 42\nf\"{x:>5}|\"") == "   42|");
    CHECK(evalStr(vm, "f\"{1000000:,}\"") == "1,000,000");
    CHECK(evalStr(vm, "f\"{[1, 2, 3][1]:03d}\"") == "002");
    // a `:` inside a slice or dict literal is NOT a spec separator
    CHECK(evalStr(vm, "var s = \"abcde\"\nf\"{s[1:3]}\"") == "bc");
    CHECK(evalStr(vm, "f\"{ {7: 8}[7] }\"") == "8");
    // surrounding whitespace inside the braces is allowed (f"{ x }")
    CHECK(evalStr(vm, "var x = 5\nf\"{ x }\"") == "5");
    CHECK(evalStr(vm, "var x = 5\nf\"a{  x  }b\"") == "a5b");

    // the SAME f-string behaves identically in all four f-flavours (f"", f'', f"""""", f'''''') and
    // the raw-f variants leave backslashes alone while still interpolating.
    {
        // (open, close) for f-string flavours whose body uses neither the closing quote nor a backslash
        struct Q { std::string open, close; };
        std::vector<Q> flav = {{"f\"", "\""}, {"f'", "'"}, {"f\"\"\"", "\"\"\""}, {"f'''", "'''"}};
        for (const auto& q : flav) {
            CHECK(evalStr(vm, "var n = 7\n" + q.open + "n is {n}" + q.close) == "n is 7");
            CHECK(evalStr(vm, "var a = 3\nvar b = 4\n" + q.open + "{a} + {b} = {a + b}" + q.close) == "3 + 4 = 7");
            CHECK(evalStr(vm, q.open + "{{esc}} {1 + 1}" + q.close) == "{esc} 2");
            CHECK(evalStr(vm, "var x = 42\n" + q.open + "{x:05d}" + q.close) == "00042");
            CHECK(evalStr(vm, q.open + "no placeholders" + q.close) == "no placeholders");
            CHECK(evalStr(vm, "var p = [5, 9]\n" + q.open + "sum is {p[0] + p[1]}" + q.close) == "sum is 14");
        }
        // raw f-strings: backslashes are literal, expression still evaluated
        CHECK(evalStr(vm, "var n = 7\nrf\"a\\t{n}\"") == "a\\t7");
        CHECK(evalStr(vm, "var n = 7\nfr'{n}\\n'") == "7\\n");
        // single-quote keys inside a double-quoted f-string (impossible before single-quote strings)
        CHECK(evalStr(vm, "var d = {\"k\": 8}\nf\"{d['k']}\"") == "8");
    }

    return RUN_TESTS();
}
