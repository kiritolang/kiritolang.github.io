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

    // length and indexing (with negative indices) — checked in every non-f literal form, since a
    // string behaves the same however it is written.
    const std::vector<std::string> openers = {"\"", "'", "\"\"\"", "'''", "r\"", "r'", "r\"\"\"", "r'''"};
    for (const std::string& q : openers) {
        // q is the OPENING delimiter; mirror it to the matching close
        std::string close = q;
        if (close.rfind("r", 0) == 0) close = close.substr(1);          // raw prefix isn't part of the close
        std::string h = q + "hello" + close, a = q + "abc" + close;
        CHECK(evalStr(vm, "len(" + h + ")") == "5");
        CHECK(evalStr(vm, a + "[0]") == "a");
        CHECK(evalStr(vm, a + "[-1]") == "c");
        CHECK_THROWS(vm.runSource(a + "[9]"));
    }

    // iteration over characters (double, single, and triple forms)
    CHECK(evalStr(vm, R"(
var s = ""
for c in "abc":
    s = s + c
s
)") == "abc");
    CHECK(evalStr(vm, R"(
var n = 0
for c in 'hello':
    n = n + 1
n
)") == "5");
    CHECK(evalStr(vm, R"(
var n = 0
for c in """hello""":
    n = n + 1
n
)") == "5");

    // REPL keeps bindings across calls (persistent scope)
    {
        KiritoVM r;
        r.runRepl("var x = 1");
        r.runRepl("x = x + 1");
        CHECK(r.stringify(r.runRepl("x")) == "2");
    }

    // The REPL's persistent scope is a module scope treated exactly like a run file under strict
    // lexical addressing: a closure defined on an earlier line captures it by handle and observes a
    // later mutation, and cross-line references resolve to stable (append-only) slots.
    {
        KiritoVM r;
        r.runRepl("var g = 10");
        r.runRepl("var f = Function(): return g");
        CHECK(r.stringify(r.runRepl("f()")) == "10");
        r.runRepl("g = 20");
        CHECK(r.stringify(r.runRepl("f()")) == "20");   // the closure sees the cross-line rebind
    }

    return RUN_TESTS();
}
