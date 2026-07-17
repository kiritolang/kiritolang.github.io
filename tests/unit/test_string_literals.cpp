// Every way of writing a string literal: plain / raw (r) / f / raw-f (rf), each in single-quote,
// double-quote, triple-single ('''), and triple-double (""") flavours. Covers equivalence across
// forms, quote nesting, escape decoding (cooked) vs verbatim (raw), triple-quoted multiline,
// f-string features in every flavour, prefixes-as-identifiers, adversarial parse errors, and
// randomized fuzz against an independent C++ oracle.
#include <random>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

// The eight non-f literal spellings of a piece of *already-escaped* body text (i.e. body is what
// goes between the quotes). Quote-style choice is the caller's concern.
struct Form {
    std::string prefix;   // "" or "r"
    std::string open;     // opening delimiter
    std::string close;    // closing delimiter
    bool raw;
    bool triple;
};
static const std::vector<Form> kPlainForms = {
    {"", "\"", "\"", false, false},
    {"", "'", "'", false, false},
    {"", "\"\"\"", "\"\"\"", false, true},
    {"", "'''", "'''", false, true},
    {"r", "\"", "\"", true, false},
    {"r", "'", "'", true, false},
    {"r", "\"\"\"", "\"\"\"", true, true},
    {"r", "'''", "'''", true, true},
};

int main() {
    // ----------------------------------------------------------- equivalence: a plain body with no
    // escapes, braces or quotes is byte-identical across all eight non-f forms (and f-forms too).
    {
        KiritoVM vm;
        for (const std::string body : {"hello", "a b c", "12345", "Unicode: café — ✓", ""}) {
            std::string ref;
            for (const auto& f : kPlainForms) {
                std::string got = evalStr(vm, f.prefix + f.open + body + f.close);
                CHECK(got == body);
            }
            // f-forms with no braces are also just the body
            CHECK(evalStr(vm, "f\"" + body + "\"") == body);
            CHECK(evalStr(vm, "f'" + body + "'") == body);
            CHECK(evalStr(vm, "f\"\"\"" + body + "\"\"\"") == body);
            CHECK(evalStr(vm, "f'''" + body + "'''") == body);
            CHECK(evalStr(vm, "rf\"" + body + "\"") == body);
            CHECK(evalStr(vm, "fr'" + body + "'") == body);
            (void)ref;
        }
    }

    // ----------------------------------------------------------- quote nesting: the other quote is an
    // ordinary character; triple forms admit lone single/double quotes freely.
    {
        KiritoVM vm;
        CHECK(evalStr(vm, "'he said \"hi\"'") == "he said \"hi\"");
        CHECK(evalStr(vm, "\"it's fine\"") == "it's fine");
        CHECK(evalStr(vm, "\"\"\"both ' and \" here\"\"\"") == "both ' and \" here");
        CHECK(evalStr(vm, "'''both ' and \" plus '' and \"\"'''") == "both ' and \" plus '' and \"\"");
        // an escaped same-quote still works in the single-quote forms
        CHECK(evalStr(vm, "'it\\'s'") == "it's");
        CHECK(evalStr(vm, "\"say \\\"hi\\\"\"") == "say \"hi\"");
    }

    // ----------------------------------------------------------- cooked escapes decode identically in
    // every cooked flavour (", ', """, ''') and in f-strings; raw flavours keep them verbatim.
    {
        KiritoVM vm;
        // body -> decoded value
        struct E { std::string body, decoded; };
        std::vector<E> escapes = {
            {"a\\nb", "a\nb"},
            {"x\\ty", "x\ty"},
            {"r\\rn", "r\rn"},
            {"back\\\\slash", "back\\slash"},
            {"hex\\x41end", "hexAend"},
            {"nul\\0z", std::string("nul\0z", 5)},
        };
        for (const auto& e : escapes) {
            CHECK(evalStr(vm, "\"" + e.body + "\"") == e.decoded);
            CHECK(evalStr(vm, "'" + e.body + "'") == e.decoded);
            CHECK(evalStr(vm, "\"\"\"" + e.body + "\"\"\"") == e.decoded);
            CHECK(evalStr(vm, "'''" + e.body + "'''") == e.decoded);
            CHECK(evalStr(vm, "f\"" + e.body + "\"") == e.decoded);    // f-strings decode escapes too
            CHECK(evalStr(vm, "f'''" + e.body + "'''") == e.decoded);
            // raw: the backslash and following char survive verbatim
            CHECK(evalStr(vm, "r\"" + e.body + "\"") == e.body);
            CHECK(evalStr(vm, "r'''" + e.body + "'''") == e.body);
            CHECK(evalStr(vm, "rf\"" + e.body + "\"") == e.body);      // raw f-string: verbatim too
        }
        // raw shields a quote from terminating but keeps both chars
        CHECK(evalStr(vm, "r\"\\\"\"") == "\\\"");                     // r"\""  -> \"
        CHECK(evalStr(vm, "len(r\"\\\"\")") == "2");
    }

    // ----------------------------------------------------------- triple-quoted multiline: literal
    // newlines are preserved; works raw and cooked, both quote chars.
    {
        KiritoVM vm;
        CHECK(evalStr(vm, "\"\"\"line1\nline2\nline3\"\"\"") == "line1\nline2\nline3");
        CHECK(evalStr(vm, "'''a\nb'''") == "a\nb");
        CHECK(evalStr(vm, "r\"\"\"raw\\nstays\ntwolines\"\"\"") == "raw\\nstays\ntwolines");
        CHECK(evalStr(vm, "len(\"\"\"ab\ncd\"\"\")") == "5");          // 2 + newline + 2
        // a multiline string used in an expression keeps surrounding code working
        CHECK(evalStr(vm, "var s = \"\"\"x\ny\"\"\"\nlen(s)") == "3");
    }

    // ----------------------------------------------------------- f-strings in every flavour: braces,
    // {{ }} escapes, format specs, and single-quote keys inside double-quoted f-strings.
    {
        KiritoVM vm;
        CHECK(evalStr(vm, "var n = 7\nf\"n={n}\"") == "n=7");
        CHECK(evalStr(vm, "var n = 7\nf'n={n}'") == "n=7");
        CHECK(evalStr(vm, "var n = 7\nf\"\"\"n={n}\"\"\"") == "n=7");
        CHECK(evalStr(vm, "var n = 7\nf'''n={n}'''") == "n=7");
        CHECK(evalStr(vm, "f\"{{lit}} {1+1}\"") == "{lit} 2");
        CHECK(evalStr(vm, "f'{{lit}} {1+1}'") == "{lit} 2");
        CHECK(evalStr(vm, "var x = 42\nf'{x:05d}'") == "00042");
        CHECK(evalStr(vm, "var pi = 3.14159\nf'''{pi:.2f}'''") == "3.14");
        // single-quote string key inside a double-quoted f-string (newly possible)
        CHECK(evalStr(vm, "var d = {\"k\": 9}\nf\"{d['k']}\"") == "9");
        CHECK(evalStr(vm, "var d = {\"k\": 9}\nf\"{d['k']:03d}\"") == "009");
        // multiline f-string with an interpolation on another line
        CHECK(evalStr(vm, "var a = 1\nvar b = 2\nf\"\"\"sum\n{a + b}\nend\"\"\"") == "sum\n3\nend");
    }

    // ----------------------------------------------------------- raw f-strings: backslashes verbatim,
    // but {expr} is still evaluated.
    {
        KiritoVM vm;
        CHECK(evalStr(vm, "var name = \"Kirito\"\nrf\"path\\to\\{name}\"") == "path\\to\\Kirito");
        CHECK(evalStr(vm, "var n = 3\nfr'{n}\\n'") == "3\\n");
        CHECK(evalStr(vm, "var n = 3\nrf\"\"\"a\\t{n}\"\"\"") == "a\\t3");
    }

    // ----------------------------------------------------------- the prefixes are still valid
    // identifiers when not followed by a quote.
    {
        KiritoVM vm;
        CHECK(evalStr(vm, "var r = 5\nr") == "5");
        CHECK(evalStr(vm, "var f = 6\nf") == "6");
        CHECK(evalStr(vm, "var rf = 7\nrf") == "7");
        CHECK(evalStr(vm, "var fr = 8\nfr") == "8");
        CHECK(evalStr(vm, "var R = 9\nR") == "9");
        CHECK(evalStr(vm, "var format = Function(x): return x\nformat(3)") == "3");  // 'format' not a prefix
    }

    // ----------------------------------------------------------- string OPERATIONS behave the same no
    // matter how the literal was written (the "every existing string test, every form" requirement).
    {
        KiritoVM vm;
        // len / index / slice / iteration / concat / methods, each driven through several forms
        CHECK(evalStr(vm, "len('hello')") == "5");
        CHECK(evalStr(vm, "len(\"\"\"hello\"\"\")") == "5");
        CHECK(evalStr(vm, "len(r'hello')") == "5");
        CHECK(evalStr(vm, "'abc'[0]") == "a");
        CHECK(evalStr(vm, "\"\"\"abc\"\"\"[-1]") == "c");
        CHECK(evalStr(vm, "r'abcde'[1:3]") == "bc");
        CHECK(evalStr(vm, "'AB' + \"\"\"CD\"\"\"") == "ABCD");
        CHECK(evalStr(vm, "'ab' * 3") == "ababab");
        CHECK(evalStr(vm, "'Hello'.upper()") == "HELLO");
        CHECK(evalStr(vm, "\"\"\"Hello World\"\"\".split()") == "['Hello', 'World']");
        CHECK(evalStr(vm, "r'a,b,c'.split(',')") == "['a', 'b', 'c']");
        CHECK(evalStr(vm, "'  pad  '.strip()") == "pad");
        CHECK(evalStr(vm, "'abc'.startswith('ab')") == "True");
        CHECK(evalStr(vm, "'-'.join(['a', 'b', 'c'])") == "a-b-c");
        // iteration is identical
        CHECK(evalStr(vm, "var n = 0\nfor c in '''abcd''':\n    n = n + 1\nn") == "4");
        // equality across forms
        CHECK(evalStr(vm, "'abc' == \"abc\"") == "True");
        CHECK(evalStr(vm, "\"\"\"abc\"\"\" == r'abc'") == "True");
        CHECK(evalStr(vm, "'a\\nb' == \"\"\"a\nb\"\"\"") == "True");   // escaped \n equals a real newline
    }

    // ----------------------------------------------------------- adversarial parse errors: every
    // form has its own unterminated case; raw and bad escapes are rejected with clear messages.
    {
        KiritoVM vm;
        CHECK_THROWS(vm.runSource("\"abc"));                 // unterminated double
        CHECK_THROWS(vm.runSource("'abc"));                  // unterminated single
        CHECK_THROWS(vm.runSource("\"\"\"abc"));             // unterminated triple-double
        CHECK_THROWS(vm.runSource("'''abc"));                // unterminated triple-single
        CHECK_THROWS(vm.runSource("r\"abc"));                // unterminated raw
        CHECK_THROWS(vm.runSource("f\"abc"));                // unterminated f
        CHECK_THROWS(vm.runSource("rf\"abc"));               // unterminated raw-f
        CHECK_THROWS(vm.runSource("\"a\nb\""));              // newline in single-line string
        CHECK_THROWS(vm.runSource("'a\nb'"));
        CHECK_THROWS(vm.runSource("r\"\\\""));               // raw ending in a shielded quote -> unterminated
        CHECK_THROWS(vm.runSource("\"\\q\""));               // invalid escape
        CHECK_THROWS(vm.runSource("\"\\xZZ\""));             // bad hex escape
        CHECK_THROWS(vm.runSource("f\"{unclosed\""));        // unmatched brace in f-string
        // an f-string is NOT closed by a quote that belongs to a nested string of the SAME kind
        CHECK_THROWS(vm.runSource("f\"{ \"x\" }\""));        // double-quote inside double-quoted f-string
    }

    // ----------------------------------------------------------- fuzz #1: random brace/quote/backslash
    // -free body round-trips identically through every plain form (and matches the C++ oracle).
    {
        std::mt19937 rng(0x57A1B2C3u);
        // printable ASCII minus the chars that need escaping or change meaning: " ' \ { }
        std::string alpha;
        for (int ch = 0x20; ch < 0x7f; ++ch)
            if (ch != '"' && ch != '\'' && ch != '\\' && ch != '{' && ch != '}')
                alpha += static_cast<char>(ch);
        std::uniform_int_distribution<int> lenD(0, 24), pick(0, static_cast<int>(alpha.size()) - 1);
        for (int iter = 0; iter < 1500; ++iter) {
            KiritoVM vm;
            std::string body;
            int len = lenD(rng);
            for (int i = 0; i < len; ++i) body += alpha[pick(rng)];
            for (const auto& f : kPlainForms)
                CHECK(evalStr(vm, f.prefix + f.open + body + f.close) == body);
            // f-forms (no braces in body) also reproduce it
            CHECK(evalStr(vm, "f\"" + body + "\"") == body);
            CHECK(evalStr(vm, "rf'''" + body + "'''") == body);
        }
    }

    // ----------------------------------------------------------- fuzz #2: random sequences of cooked
    // escapes + literals decode the same in a double-quoted, single-quoted, and triple string, and a
    // raw string keeps the source bytes verbatim. Oracle is computed in C++.
    {
        std::mt19937 rng(0xBEEF77u);
        struct Esc { const char* src; const char* val; };
        std::vector<Esc> table = {
            {"\\n", "\n"}, {"\\t", "\t"}, {"\\r", "\r"}, {"\\\\", "\\"},
            {"\\x41", "A"}, {"\\x7e", "~"}, {"\\0", ""},  // \0's value is special-cased below
        };
        // safe literal chars (no quote/backslash/brace/newline)
        std::string lit = "abcXYZ0189 .,-_/=+:;";
        std::uniform_int_distribution<int> chunks(0, 8), kind(0, 1),
            escPick(0, static_cast<int>(table.size()) - 1), litPick(0, static_cast<int>(lit.size()) - 1);
        for (int iter = 0; iter < 1500; ++iter) {
            KiritoVM vm;
            std::string body, oracle;
            int n = chunks(rng);
            bool hasNul = false;
            for (int i = 0; i < n; ++i) {
                if (kind(rng) == 0) {
                    const Esc& e = table[escPick(rng)];
                    body += e.src;
                    std::string v = (std::string(e.src) == "\\0") ? std::string(1, '\0') : std::string(e.val);
                    if (std::string(e.src) == "\\0") hasNul = true;
                    oracle += v;
                } else {
                    char c = lit[litPick(rng)];
                    body += c;
                    oracle += c;
                }
            }
            // cooked forms all decode to the same oracle
            CHECK(evalStr(vm, "\"" + body + "\"") == oracle);
            CHECK(evalStr(vm, "'" + body + "'") == oracle);
            CHECK(evalStr(vm, "\"\"\"" + body + "\"\"\"") == oracle);
            // a raw string of the same source keeps the bytes verbatim (no NUL to confuse it)
            if (!hasNul)
                CHECK(evalStr(vm, "r\"" + body + "\"") == body);
        }
    }

    return RUN_TESTS();
}
