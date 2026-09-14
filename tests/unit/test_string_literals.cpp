// Every way of writing a string literal: plain / raw (r) / f / raw-f (rf), each in single-quote,
// double-quote, triple-single ('''), and triple-double (""") flavours. The deterministic,
// hand-written-expected cases (equivalence across forms, quote nesting, escape decoding, triple-quoted
// multiline, f-string features, prefixes-as-identifiers, string operations) live in
// tests/scripts/unit_string_literals.ki; the 14 adversarial parse-error cases live in
// tests/errors/unit_string_literals_*.ki. What remains HERE are the two seeded randomized-fuzz blocks,
// whose oracle is computed at runtime (not hand-written), so they aren't a fit for the .ki conversion.
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
