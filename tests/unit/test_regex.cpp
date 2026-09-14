// C++-only remnant of the `regex` module test (see tests/scripts/unit_regex.ki for the public-API
// coverage; this file keeps only what genuinely can't run as a Kirito script):
//   - a linear-time timing stress: asserts wall-clock milliseconds, which is inherently
//     nondeterministic / machine-speed-dependent and not something a golden .ki/.expected diff can
//     pin.
//   - a property-based fuzz that drives the internal reng:: engine API directly (raw
//     Program/MatchResult/compile/run/toCodepoints) -- not reachable from Kirito script, which only
//     sees the module's compile/search/match/... surface, not the engine's internal slot array.
#include <chrono>
#include <random>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"
#include "kirito/regex_engine.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
// run a snippet with the regex module already imported as `re` and `io`
static std::string re(KiritoVM& vm, const std::string& expr) {
    return evalStr(vm, "var io = import(\"io\")\nvar re = import(\"regex\")\n" + expr);
}

int main() {
    // -------------------------------------------------- linear time: no catastrophic backtracking
    {
        KiritoVM vm;
        // (a+)+b on a long run of 'a' with no trailing b: exponential for a backtracker, instant here.
        auto start = std::chrono::steady_clock::now();
        CHECK(re(vm, "re.search(\"(a+)+b\", \"a\" * 6000 + \"c\")") == "None");
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        CHECK(ms < 5000);   // generously bounded; a backtracking engine never finishes
    }

    // -------------------------------------------------- property-based fuzz on the engine
    {
        using namespace reng;
        std::mt19937 rng(0x9E3779B9u);
        const char* atoms[] = {"a", "b", "c", ".", "\\d", "\\w", "[ab]", "[^a]", "(a|b)", "x"};
        const char* quants[] = {"", "*", "+", "?", "*?", "+?", "{1,3}", "{2}"};
        for (int iter = 0; iter < 3000; ++iter) {
            // build a small random pattern
            std::string pat;
            int parts = 1 + (rng() % 4);
            for (int i = 0; i < parts; ++i) {
                pat += atoms[rng() % 10];
                pat += quants[rng() % 8];
                if (i + 1 < parts && (rng() % 4 == 0)) pat += "|";
            }
            Program prog;
            try { prog = compile(pat, (rng() % 2) ? IGNORECASE : 0); }
            catch (const RegexError&) { continue; }   // some random patterns are invalid; skip
            // random input over a small alphabet
            std::string in;
            int len = static_cast<int>(rng() % 12);
            for (int i = 0; i < len; ++i) in += static_cast<char>('a' + (rng() % 4));
            auto text = toCodepoints(in);
            MatchResult r = run(prog, text, 0, false, false);   // must terminate (linear time)
            if (r.matched) {
                // invariants: whole-match span is well-formed and within bounds
                int a = r.slots[0], b = r.slots[1];
                CHECK(a >= 0 && a <= b && b <= static_cast<int>(text.size()));
                // every participating group lies within the whole match
                for (int g = 1; g <= prog.numGroups; ++g) {
                    int ga = r.slots[2 * g], gb = r.slots[2 * g + 1];
                    if (ga >= 0) CHECK(ga >= a && ga <= gb && gb <= b);
                }
                // the matched substring, fullmatched against the same pattern, still matches
                std::vector<int32_t> sub(text.begin() + a, text.begin() + b);
                CHECK(run(prog, sub, 0, true, true).matched);
            }
        }
    }

    return RUN_TESTS();
}
