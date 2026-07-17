// List ordering/concatenation (lexicographic) and the string-indexing fast path
// (O(1) ASCII indexing — a perf cliff found via the SQL example project). Behavior + a light
// scaling sanity check.
#include <chrono>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; } catch (const KiritoError&) { return true; }
}

int main() {
    // --- list lexicographic ordering ---
    {
        KiritoVM vm;
        CHECK(run(vm, "String([1, 2] < [1, 3])") == "True");
        CHECK(run(vm, "String([1, 2] < [1, 2, 0])") == "True");   // shared prefix: shorter is less
        CHECK(run(vm, "String([2] < [1, 9])") == "False");        // first element decides
        CHECK(run(vm, "String([1, 2] <= [1, 2])") == "True");
        CHECK(run(vm, "String([1, 2] >= [1, 2])") == "True");
        CHECK(run(vm, "String([1, 3] > [1, 2])") == "True");
        CHECK(run(vm, "String([\"a\", \"b\"] < [\"a\", \"c\"])") == "True");  // string elements
        CHECK(run(vm, "String([[1], [2]] < [[1], [3]])") == "True");          // nested lists
        // ordering a List against a non-List throws
        CHECK(throws(vm, "[1, 2] < 3"));
        CHECK(throws(vm, "[1] < \"a\""));
    }

    // --- list concatenation ---
    {
        KiritoVM vm;
        CHECK(run(vm, "String([1, 2] + [3, 4])") == "[1, 2, 3, 4]");
        CHECK(run(vm, "String([] + [1])") == "[1]");
        CHECK(run(vm, "var a = [1]\nvar b = a + [2]\nString(a)") == "[1]");  // + does not mutate lhs
        CHECK(throws(vm, "[1] + 2"));   // can't concat List and non-List
    }

    // --- multi-key sort via list-returning key (the idiom list ordering enables) ---
    {
        KiritoVM vm;
        // sort by (parity, value): evens first, ascending within each group
        CHECK(run(vm, "String(sorted([5, 2, 4, 1, 3], Function(x): return [x % 2, x]))") == "[2, 4, 1, 3, 5]");
    }

    // --- string indexing is O(1) on ASCII: a char-by-char scan of a big string is fast/linear ---
    {
        KiritoVM vm;
        // build a 20000-char string, then index every position; if indexing were O(n) this is O(n^2).
        auto t0 = std::chrono::steady_clock::now();
        std::string out = run(vm,
            "var s = \"a\" * 20000\n"
            "var total = 0\n"
            "var i = 0\n"
            "while i < len(s):\n"
            "    if s[i] == \"a\":\n"
            "        total = total + 1\n"
            "    i = i + 1\n"
            "total");
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        CHECK(out == "20000");
        CHECK(ms < 2000);  // generous bound; the O(n^2) version took many seconds at this size
    }

    // --- Unicode indexing still correct (cached code-point table path) ---
    {
        KiritoVM vm;
        CHECK(run(vm, "\"héllo\"[1]") == "é");
        CHECK(run(vm, "\"żółć\"[3]") == "ć");
        CHECK(run(vm, "len(\"chrząszcz\")") == "9");
        CHECK(run(vm, "\"abcdef\"[1:4]") == "bcd");
        CHECK(run(vm, "\"żółćabc\"[2:5]") == "łća");
    }

    return RUN_TESTS();
}
