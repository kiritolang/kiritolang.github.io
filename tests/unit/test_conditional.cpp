// Conditional expression: `then if cond else orelse`. The deterministic value + edge cases were
// migrated to tests/scripts/unit_conditional.ki (a golden round-trip). What remains here are the
// non-convertible cases: a generative deep else-chain that must throw rather than crash, and two
// randomized fuzz sweeps that check evaluation against an independent C++ oracle.
#include <random>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    // ---------------------------------------------------------------- adversarial parse errors
    {
        KiritoVM vm;
        CHECK_THROWS(vm.runSource("1 if True"));          // missing else
        CHECK_THROWS(vm.runSource("1 if else 2"));        // missing condition
        CHECK_THROWS(vm.runSource("if True else 2"));     // missing then (parses as an if-statement head)
        CHECK_THROWS(vm.runSource("1 if True else"));     // missing orelse
        CHECK_THROWS(vm.runSource("1 else 2"));           // else without if
        // a pathologically deep else-chain throws a clean parse error, never crashes
        std::string chain = "9";
        for (int i = 0; i < 8000; ++i) chain = "0 if False else " + chain;
        CHECK_THROWS(vm.runSource(chain + "\n"));
    }

    // ---------------------------------------------------------------- randomized fuzz vs a C++ oracle
    {
        std::mt19937 rng(0xC0FFEE);
        std::uniform_int_distribution<int> lenD(1, 6), valD(0, 999), boolD(0, 1);
        for (int iter = 0; iter < 4000; ++iter) {
            KiritoVM vm;
            int len = lenD(rng);
            std::vector<int> values;
            std::vector<bool> conds;
            // Build  v0 if c0 else v1 if c1 else ... else vLen  (the chain has `len` conditions and
            // `len + 1` values), then compute the expected value the same way Kirito should.
            std::string src;
            for (int i = 0; i < len; ++i) {
                int v = valD(rng);
                bool c = boolD(rng) == 1;
                values.push_back(v);
                conds.push_back(c);
                src += std::to_string(v) + (c ? " if True else " : " if False else ");
            }
            int last = valD(rng);
            values.push_back(last);
            src += std::to_string(last);

            // Oracle: the first value whose condition is true, else the final value.
            int expected = last;
            for (int i = 0; i < len; ++i)
                if (conds[i]) { expected = values[i]; break; }

            CHECK(evalStr(vm, src) == std::to_string(expected));
        }
    }

    // ---------------------------------------------------------------- fuzz: truthiness of varied conds
    {
        std::mt19937 rng(12345);
        const char* condForms[] = {"0", "1", "\"\"", "\"x\"", "[]", "[1]", "None", "2 - 2", "3 % 2"};
        const bool truthy[] = {false, true, false, true, false, true, false, false, true};
        std::uniform_int_distribution<int> pick(0, 8);
        for (int iter = 0; iter < 2000; ++iter) {
            KiritoVM vm;
            int k = pick(rng);
            std::string src = std::string("\"T\" if ") + condForms[k] + " else \"F\"";
            CHECK(evalStr(vm, src) == (truthy[k] ? "T" : "F"));
        }
    }

    return RUN_TESTS();
}
