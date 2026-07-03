#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;

    // --- Unicode (indexing/length by code point, not byte) ---
    CHECK(evalStr(vm, "len(\"h\xc3\xa9llo\")") == "5");          // héllo: 6 bytes, 5 code points
    CHECK(evalStr(vm, "\"h\xc3\xa9llo\"[1]") == "\xc3\xa9");      // -> é
    CHECK(evalStr(vm, "len(\"ab\xe2\x82\xac\")") == "3");        // ab€ (€ is 3 bytes)
    CHECK(evalStr(vm, "\"ab\xe2\x82\xac\"[2]") == "\xe2\x82\xac");
    CHECK(evalStr(vm, R"(
var n = 0
for c in "ab€":
    n = n + 1
n
)") == "3");

    // --- slicing (String) ---
    CHECK(evalStr(vm, "\"hello\"[1:4]") == "ell");
    CHECK(evalStr(vm, "\"hello\"[:3]") == "hel");
    CHECK(evalStr(vm, "\"hello\"[2:]") == "llo");
    CHECK(evalStr(vm, "\"hello\"[-2:]") == "lo");
    CHECK(evalStr(vm, "\"hello\"[::-1]") == "olleh");
    CHECK(evalStr(vm, "\"hello\"[::2]") == "hlo");

    // --- slicing (List) ---
    CHECK(evalStr(vm, "[1, 2, 3, 4, 5][1:4]") == "[2, 3, 4]");
    CHECK(evalStr(vm, "[1, 2, 3][::-1]") == "[3, 2, 1]");

    // --- in / not in ---
    CHECK(evalStr(vm, "\"ell\" in \"hello\"") == "True");
    CHECK(evalStr(vm, "\"z\" in \"hello\"") == "False");
    CHECK(evalStr(vm, "2 in [1, 2, 3]") == "True");
    CHECK(evalStr(vm, "5 not in [1, 2, 3]") == "True");
    CHECK(evalStr(vm, "\"a\" in {\"a\": 1}") == "True");
    CHECK(evalStr(vm, "2 in {1, 2, 3}") == "True");

    // --- repetition ---
    CHECK(evalStr(vm, "\"ab\" * 3") == "ababab");

    // --- methods ---
    CHECK(evalStr(vm, "\"Hello\".upper()") == "HELLO");
    CHECK(evalStr(vm, "\"Hello\".lower()") == "hello");
    CHECK(evalStr(vm, "\"  hi  \".strip()") == "hi");
    CHECK(evalStr(vm, "len(\"a,b,c\".split(\",\"))") == "3");
    CHECK(evalStr(vm, "\"a,b,c\".split(\",\")[1]") == "b");
    CHECK(evalStr(vm, "\",\".join([\"a\", \"b\", \"c\"])") == "a,b,c");
    CHECK(evalStr(vm, "\"hello\".replace(\"l\", \"L\")") == "heLLo");
    CHECK(evalStr(vm, "\"hello\".startswith(\"he\")") == "True");
    CHECK(evalStr(vm, "\"hello\".endswith(\"lo\")") == "True");
    CHECK(evalStr(vm, "\"hello\".find(\"ll\")") == "2");
    CHECK(evalStr(vm, "\"banana\".count(\"a\")") == "3");

    // --- formatting ---
    CHECK(evalStr(vm, "\"{} + {} = {}\".format(1, 2, 3)") == "1 + 2 = 3");
    CHECK(evalStr(vm, "\"{0} {0}\".format(\"hi\")") == "hi hi");
    CHECK(evalStr(vm, "\"{}%\".format(50)") == "50%");

    // --- every operation/method gives the SAME result no matter how the subject literal is written:
    // double / single / triple-double / triple-single, plain or raw. The body has no escapes, so the
    // raw and cooked spellings denote the same string.
    {
        // wrap a quote-free, escape-free body in each of the eight non-f literal forms
        auto forms = [](const std::string& body) {
            return std::vector<std::string>{
                "\"" + body + "\"", "'" + body + "'",
                "\"\"\"" + body + "\"\"\"", "'''" + body + "'''",
                "r\"" + body + "\"", "r'" + body + "'",
                "r\"\"\"" + body + "\"\"\"", "r'''" + body + "'''",
            };
        };
        for (const std::string& s : forms("Hello")) {
            CHECK(evalStr(vm, s + ".upper()") == "HELLO");
            CHECK(evalStr(vm, s + ".lower()") == "hello");
            CHECK(evalStr(vm, "len(" + s + ")") == "5");
            CHECK(evalStr(vm, s + "[0]") == "H");
            CHECK(evalStr(vm, s + "[-1]") == "o");
            CHECK(evalStr(vm, s + "[1:4]") == "ell");
            CHECK(evalStr(vm, s + "[::-1]") == "olleH");
            CHECK(evalStr(vm, s + " * 2") == "HelloHello");
            CHECK(evalStr(vm, s + ".startswith('He')") == "True");
            CHECK(evalStr(vm, "'ell' in " + s) == "True");
            CHECK(evalStr(vm, s + " == 'Hello'") == "True");
        }
        for (const std::string& s : forms("a,b,c")) {
            CHECK(evalStr(vm, "len(" + s + ".split(','))") == "3");
            CHECK(evalStr(vm, s + ".split(',')[1]") == "b");
            CHECK(evalStr(vm, s + ".replace(',', '-')") == "a-b-c");
            CHECK(evalStr(vm, s + ".count(',')") == "2");
        }
        for (const std::string& s : forms("  hi  "))
            CHECK(evalStr(vm, s + ".strip()") == "hi");
        // .format works the same from any form, and an f-string reproduces the same result
        for (const std::string& s : forms("{} + {} = {}"))
            CHECK(evalStr(vm, s + ".format(1, 2, 3)") == "1 + 2 = 3");
        CHECK(evalStr(vm, "var x = 5\nf'{x} + {x}'") == "5 + 5");
        CHECK(evalStr(vm, "var x = 5\nf\"\"\"{x} + {x}\"\"\"") == "5 + 5");
    }

    // --- Levenshtein (Unicode edit distance), scalar + List forms ---
    CHECK(evalStr(vm, "\"kitten\".levenshtein(\"sitting\")") == "3");
    CHECK(evalStr(vm, "\"abc\".levenshtein(\"abc\")") == "0");
    CHECK(evalStr(vm, "\"\".levenshtein(\"abc\")") == "3");
    CHECK(evalStr(vm, "\"caf\xc3\xa9\".levenshtein(\"cafe\")") == "1");      // é vs e: one code point
    CHECK(evalStr(vm, "\"a\xf0\x9f\x98\x80\".levenshtein(\"a\")") == "1");   // a + emoji vs a: one edit
    CHECK(evalStr(vm, "\"kitten\".levenshtein([\"sitting\", \"kitten\", \"kit\"])") == "[3, 0, 3]");
    CHECK(evalStr(vm, "\"cat\".levenshtein(other = \"cut\")") == "1");       // keyword arg
    CHECK_THROWS(vm.runSource("\"abc\".levenshtein(5)"));                     // bad argument type
    CHECK_THROWS(vm.runSource("\"abc\".levenshtein([\"ok\", 7])"));          // non-String in List
    // the `string` module's fuzzy helpers, built on it
    CHECK(evalStr(vm, "import(\"string\").closest(\"swft\", [\"swift\", \"ruby\", \"rust\"])") == "swift");
    CHECK(evalStr(vm, "import(\"string\").similarity(\"abc\", \"abc\")") == "1.0");

    return RUN_TESTS();
}
