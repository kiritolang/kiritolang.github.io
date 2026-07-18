// Static-analysis warnings: unused variables, ignored non-None results, and the `discard` keyword.
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Analyze a source string and return the warning messages (without file:line:col prefixes).
static std::vector<std::string> warn(const std::string& src) {
    Parser parser(Lexer(src).tokenize());
    ast::Program program = parser.parseProgram();
    Analyzer analyzer;
    std::vector<std::string> msgs;
    for (const auto& w : analyzer.analyze(program)) msgs.push_back(w.message);
    return msgs;
}
static bool has(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v) if (s.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    // --- unused local variable (inside a function) ---
    {
        auto w = warn("var f = Function():\n    var unused = 5\n    return 1\n");
        CHECK(has(w, "variable 'unused' is assigned but never used"));
    }
    // a used local does not warn
    {
        auto w = warn("var f = Function():\n    var x = 5\n    return x\n");
        CHECK(!has(w, "never used"));
    }
    // module-level names are exports, never flagged
    {
        auto w = warn("var exported = 5\n");
        CHECK(!has(w, "never used"));
    }
    // class members are never flagged
    {
        auto w = warn("class C:\n    var method = Function(self):\n        return 1\n");
        CHECK(!has(w, "never used"));
    }
    // parameters are never flagged as unused
    {
        auto w = warn("var f = Function(a, b):\n    return a\n");
        CHECK(!has(w, "never used"));
    }
    // private _names are never flagged
    {
        auto w = warn("var f = Function():\n    var _scratch = 5\n    return 1\n");
        CHECK(!has(w, "never used"));
    }

    // --- ignored non-None result ---
    {
        auto w = warn("var x = 1\nx + 2\n");  // bare arithmetic
        CHECK(has(w, "result of expression is unused"));
    }
    {
        auto w = warn("var a = [1, 2, 3]\na[0]\n");  // bare index
        CHECK(has(w, "result of expression is unused"));
    }
    {
        auto w = warn("var compute = Function() -> Integer:\n    return 5\ncompute()\n");
        CHECK(has(w, "result of expression is unused"));
    }
    // a None-returning function call is NOT flagged
    {
        auto w = warn("var act = Function() -> None:\n    return None\nact()\n");
        CHECK(!has(w, "result of expression is unused"));
    }
    // an unannotated function whose last statement is a bare return is treated as None-returning
    {
        auto w = warn("var act = Function():\n    var x = 1\n    return\nact()\n");
        CHECK(!has(w, "result of expression is unused"));
    }
    // calls to unknown/native functions are left alone (we can't know their return type)
    {
        auto w = warn("var io = import(\"io\")\nio.print(\"hi\")\n");
        CHECK(!has(w, "result of expression is unused"));
    }

    // --- discard suppresses the unused-result warning ---
    {
        auto w = warn("var compute = Function() -> Integer:\n    return 5\ndiscard compute()\n");
        CHECK(!has(w, "result of expression is unused"));
    }
    {
        auto w = warn("var x = 1\ndiscard x + 2\n");
        CHECK(!has(w, "result of expression is unused"));
    }

    // --- re-declaration within a block ---
    {
        auto w = warn("var f = Function():\n    var x = 1\n    var x = 2\n    return x\n");
        CHECK(has(w, "variable 'x' is re-declared in this block"));
    }
    // re-declaring in sibling branches is fine (different blocks)
    {
        auto w = warn("var f = Function(c):\n    if c:\n        var x = 1\n    else:\n        var x = 2\n    return 0\n");
        CHECK(!has(w, "re-declared"));
    }

    // --- unreachable code after a terminator ---
    {
        auto w = warn("var f = Function():\n    return 1\n    var x = 2\n");
        CHECK(has(w, "unreachable code"));
    }
    {
        auto w = warn("var f = Function():\n    for i in [1, 2]:\n        break\n        var x = 9\n    return 0\n");
        CHECK(has(w, "unreachable code"));
    }
    // a final return is not "unreachable"
    {
        auto w = warn("var f = Function():\n    var x = 1\n    return x\n");
        CHECK(!has(w, "unreachable code"));
    }

    // --- self-assignment ---
    {
        auto w = warn("var x = 1\nx = x\n");
        CHECK(has(w, "self-assignment of 'x' has no effect"));
    }
    {
        auto w = warn("var a = 1\nvar b = 2\na = b\n");
        CHECK(!has(w, "self-assignment"));
    }

    // --- duplicate parameter name is a hard PARSE error now (not a warn-and-run) ---
    {
        bool threw = false;
        try {
            Parser(Lexer("var f = Function(a, b, a):\n    return a\n").tokenize()).parseProgram();
        } catch (const KiritoError& e) {
            threw = true;
            CHECK(std::string(e.what()).find("duplicate parameter name 'a'") != std::string::npos);
        }
        CHECK(threw);
    }
    {
        auto w = warn("var f = Function(a, b):\n    return a + b\n");
        CHECK(!has(w, "duplicate parameter"));   // distinct params: parses fine, no warning
    }

    // --- discard still runs the expression (side effects preserved) ---
    {
        KiritoVM vm;
        CHECK(vm.stringify(vm.runSource(
            "var io = import(\"io\")\nvar log = []\nvar f = Function():\n    log.append(1)\n    return 9\n"
            "discard f()\nlen(log)")) == "1");
    }

    // --- todo: warns with an optional reminder message ---
    {
        auto w = warn("todo \"wire up the cache\"\n");
        CHECK(has(w, "todo: wire up the cache"));
    }
    // bare todo warns with a default reminder
    {
        auto w = warn("todo\n");
        CHECK(has(w, "todo: not yet implemented"));
    }
    // todo inside a function body still warns
    {
        auto w = warn("var f = Function():\n    todo \"finish me\"\n    return 0\n");
        CHECK(has(w, "todo: finish me"));
    }
    // pass never warns (a plain no-op)
    {
        auto w = warn("pass\nvar f = Function():\n    pass\n    return 1\n");
        CHECK(!has(w, "todo"));
    }
    // the warning carries the todo's line:col location
    {
        Parser parser(Lexer("var x = 1\nif x == 1:\n    todo \"here\"\n").tokenize());
        ast::Program program = parser.parseProgram();
        Analyzer analyzer;
        auto formatted = formatWarnings(analyzer.analyze(program), "f.ki");
        bool found = false;
        for (const auto& s : formatted)
            if (s == "f.ki:3:5: warning: todo: here") found = true;
        CHECK(found);
    }
    // todo / pass are runtime no-ops (the program runs and produces a value)
    {
        KiritoVM vm;
        Handle r = vm.runSource("var f = Function():\n    pass\n    todo \"later\"\n    return 7\n    pass\nf()\n");
        CHECK(vm.stringify(r) == "7");
    }

    // v1.16.1 F02-1: a local READ before its declaration in source order — captured by an EARLIER-defined
    // nested function — must NOT be spuriously flagged "assigned but never used" (the analyzer once
    // dropped the markUsed because the local wasn't declared yet). The resolver resolves it fine.
    {
        auto w = warn("var outer = Function():\n"
                      "    var g = Function(): return y\n"   // g captures y, defined AFTER g
                      "    var y = 5\n"
                      "    return g()\n");
        CHECK(!has(w, "never used"));   // neither y nor g
    }
    // mutual recursion between two nested functions (CLAUDE.md says this must resolve) — no false warning
    {
        auto w = warn("var run = Function():\n"
                      "    var isEven = Function(n): return True if n == 0 else isOdd(n - 1)\n"
                      "    var isOdd = Function(n): return False if n == 0 else isEven(n - 1)\n"
                      "    return isEven(10)\n");
        CHECK(!has(w, "never used"));   // isOdd is referenced by isEven, defined before it
    }
    // ...but a genuinely-unused local ALONGSIDE a forward-captured one is STILL flagged (no false negative)
    {
        auto w = warn("var outer = Function():\n"
                      "    var g = Function(): return y\n"
                      "    var y = 5\n"
                      "    var dead = 99\n"
                      "    return g()\n");
        CHECK(has(w, "variable 'dead' is assigned but never used"));
        CHECK(!has(w, "'y' is assigned"));
    }

    return RUN_TESTS();
}
