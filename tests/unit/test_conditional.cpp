// Conditional expression: `then if cond else orelse`. Edge cases, adversarial parse cases, and a
// randomized fuzz sweep that checks evaluation against an independent C++ oracle.
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
    // ---------------------------------------------------------------- basic + edge cases
    {
        KiritoVM vm;
        CHECK(evalStr(vm, "1 if True else 2") == "1");
        CHECK(evalStr(vm, "1 if False else 2") == "2");
        // the condition uses truthiness, not just Bool: falsy values pick the else-branch
        CHECK(evalStr(vm, "\"t\" if 0 else \"f\"") == "f");
        CHECK(evalStr(vm, "\"t\" if 1 else \"f\"") == "t");
        CHECK(evalStr(vm, "\"t\" if \"\" else \"f\"") == "f");      // empty String is falsy
        CHECK(evalStr(vm, "\"t\" if \"x\" else \"f\"") == "t");
        CHECK(evalStr(vm, "\"t\" if [] else \"f\"") == "f");        // empty List is falsy
        CHECK(evalStr(vm, "\"t\" if [0] else \"f\"") == "t");
        CHECK(evalStr(vm, "\"t\" if None else \"f\"") == "f");      // None is falsy
        // branches may be different types
        CHECK(evalStr(vm, "42 if True else \"text\"") == "42");
        CHECK(evalStr(vm, "42 if False else \"text\"") == "text");
    }

    // ---------------------------------------------------------------- precedence & associativity
    {
        KiritoVM vm;
        // arithmetic binds tighter than the conditional: (1 + 2) if True else 99
        CHECK(evalStr(vm, "1 + 2 if True else 99") == "3");
        // comparison forms the condition naturally
        CHECK(evalStr(vm, "10 if 2 < 3 else 20") == "10");
        // `or` binds tighter: (False or True) if False else 7  ->  7
        CHECK(evalStr(vm, "False or True if False else 7") == "7");
        // `not` binds tighter: (not False) if True else False  ->  True
        CHECK(evalStr(vm, "not False if True else False") == "True");
        // right-associative chaining: a if c1 else b if c2 else c
        CHECK(evalStr(vm, "\"a\" if False else \"b\" if True else \"c\"") == "b");
        CHECK(evalStr(vm, "\"a\" if False else \"b\" if False else \"c\"") == "c");
        CHECK(evalStr(vm, "\"a\" if True else \"b\" if True else \"c\"") == "a");
        // a conditional whose condition is itself a conditional
        CHECK(evalStr(vm, "1 if (True if False else True) else 2") == "1");
        // conditionals in both branches
        CHECK(evalStr(vm, "(10 if False else 11) if True else (20 if True else 21)") == "11");
    }

    // ---------------------------------------------------------------- short-circuit (no eval of untaken)
    {
        KiritoVM vm;
        // the untaken branch may reference an undefined name and must not be evaluated
        CHECK(evalStr(vm, "7 if True else 1 // 0") == "7");
        CHECK(evalStr(vm, "1 // 0 if False else 7") == "7");
        // a side effect in the untaken branch must not happen
        CHECK(evalStr(vm, R"(
var log = []
var note = Function(x):
    log.append(x)
    return x
discard (note(1) if True else note(2))
log
)") == "[1]");
        CHECK(evalStr(vm, R"(
var log = []
var note = Function(x):
    log.append(x)
    return x
discard (note(1) if False else note(2))
log
)") == "[2]");
    }

    // ---------------------------------------------------------------- usable in every value position
    {
        KiritoVM vm;
        CHECK(evalStr(vm, "[1 if True else 0, 2 if False else 9, 3]") == "[1, 9, 3]");
        CHECK(evalStr(vm, "{\"k\": 1 if True else 0}[\"k\"]") == "1");
        CHECK(evalStr(vm, "abs(-5 if 1 > 0 else 5)") == "5");
        CHECK(evalStr(vm, "[10, 20, 30][1 if True else 2]") == "20");      // as an index
        CHECK(evalStr(vm, "([10, 20, 30, 40])[1 if False else 2 : 4]") == "[30, 40]");  // slice bound
        // as a return value
        CHECK(evalStr(vm, R"(
var pick = Function(flag):
    return "on" if flag else "off"
pick(True) + pick(False)
)") == "onoff");
        // as a default parameter value (evaluated at definition with the default expression)
        CHECK(evalStr(vm, R"(
var f = Function(x, label = "pos" if True else "neg"):
    return label
f(0)
)") == "pos");
        // packing: a bare comma sequence treats each element independently
        CHECK(evalStr(vm, "1, 2 if True else 3") == "[1, 2]");
        CHECK(evalStr(vm, "1, 2 if False else 3") == "[1, 3]");
    }

    // ---------------------------------------------------------------- a statement-level `if` after a
    // block is NOT a ternary. A block-bodied Function literal self-terminates at its dedent, so the
    // following `if` begins the next statement (the common `var f = Function(): ...` then `if argmain:`
    // idiom) — it must not be swallowed as a conditional-expression continuation of the function value.
    {
        KiritoVM vm;
        CHECK(evalStr(vm, R"(
var made = None
var run = Function():
    return 7
if True:
    made = run()
made
)") == "7");
        // same after other suite-closing statements (a for-loop body, a class body)
        CHECK(evalStr(vm, R"(
var total = 0
for i in [1, 2, 3]:
    total = total + i
var doubled = total * 2 if True else 0
doubled
)") == "12");
        // an INLINE function still works as a ternary operand (no block closed, so `if` IS the ternary)
        CHECK(evalStr(vm, "(Function(): return 1)() if True else 2") == "1");
    }

    // ---------------------------------------------------------------- adversarial parse errors
    {
        KiritoVM vm;
        CHECK_THROWS(vm.runSource("1 if True"));          // missing else
        CHECK_THROWS(vm.runSource("1 if else 2"));        // missing condition
        CHECK_THROWS(vm.runSource("if True else 2"));     // missing then (parses as an if-statement head)
        CHECK_THROWS(vm.runSource("1 if True else"));     // missing orelse
        CHECK_THROWS(vm.runSource("1 else 2"));           // else without if
        // a pathologically deep else-chain throws a clean parse error, never crashes
        std::string deep = "var z = 1";
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
