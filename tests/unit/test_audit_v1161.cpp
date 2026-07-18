// Regression tests for the v1.16.1 audit round. Each block pins ONE confirmed finding; every
// scenario runs in its own VM and self-asserts inside Kirito (or checks a parse-time diagnostic),
// so a regression fails the test. Each was verified to fail on the pre-fix build.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run a program; true iff it completes without throwing (asserts inside throw on failure).
static bool ok(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return true; } catch (...) { return false; }
}
// Run a program expected to throw; return the exception message (empty if it didn't throw).
static std::string errmsg(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return ""; }
    catch (const std::exception& e) { return e.what(); }
    catch (...) { return "<non-std exception>"; }
}
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
// Parse a source; true iff parsing threw a KiritoError whose message contains `needle`.
static bool parseThrows(const std::string& src, const std::string& needle) {
    try {
        Parser(Lexer(src).tokenize()).parseProgram();
        return false;
    } catch (const KiritoError& e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
}

int main() {
    // ===== F01-1: comparison operators do NOT chain. `a < b < c` would parse as `(a < b) < c` and
    // compare a Bool — a footgun. Reject a second comparison-family operator at parse time and steer
    // the user to `and`/`or`. The parenthesized/and-joined forms still parse. =====
    {
        CHECK(parseThrows("var a = 1\nvar b = 2\nvar c = 3\ndiscard a < b < c\n", "chained comparison is not allowed"));
        CHECK(parseThrows("discard 1 == 1 == 1\n", "chained comparison is not allowed"));
        CHECK(parseThrows("discard 1 <= 2 >= 0\n", "chained comparison is not allowed"));
        CHECK(parseThrows("var xs = [1]\ndiscard 1 in xs in xs\n", "chained comparison is not allowed"));
        CHECK(parseThrows("var xs = [1]\ndiscard 1 not in xs == True\n", "chained comparison is not allowed"));
        // valid non-chained forms parse cleanly
        KiritoVM vm;
        CHECK(ok(vm, "assert (1 < 2) and (2 < 3)\nassert (1 == 1) == True\nassert 1 < 2 and 2 < 3 and 3 < 4\n"));
        CHECK(ok(vm, "var xs = [1, 2]\nassert (1 in xs) == True\nassert 1 in xs and 2 in xs\n"));
    }

    // ===== F07-9: sum() folds numeric value types (BigInt/Complex, ValueKind::Instance) via a generic
    // accumulator, but must NOT concatenate Strings/Lists — the deliberate "sum expects numbers" /
    // "sum start must be a number" contract. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var int = import("int")
var complex = import("complex")
# generic fold works for numeric value types
assert String(sum([int.big(5), int.big(3), int.big(2)])) == "10"
assert String(sum([int.big(5), 3, 2])) == "10"
assert sum([complex.of(1, 1), complex.of(2, 2)], complex.zero) == complex.of(3, 3)
# scalar fast path unchanged
assert sum([1, 2, 3]) == 6 and sum([1.5, 2.5]) == 4.0 and sum([]) == 0
)"));
        // ...but non-numbers throw, both as elements and as the start
        CHECK(!ok(vm, "discard sum([\"a\", \"b\"])\n"));
        CHECK(!ok(vm, "discard sum([[1], [2]])\n"));
        CHECK(!ok(vm, "discard sum([[1], [2]], [])\n"));
        CHECK(!ok(vm, "discard sum([1, 2], \"x\")\n"));
    }

    // ===== F01-2: a trailing comma after a single target forces a 1-element unpack (like the bare
    // `a, = x`), NOT a silent whole-iterable bind. So `var a, = [7]` binds a=7 and `var a, = [1,2]`
    // throws a count mismatch; `for x, in [[1],[2]]` unpacks each 1-element item. =====
    {
        KiritoVM vm;
        CHECK(vm.stringify(vm.runSource("var a, = [7]\na\n")) == "7");
        CHECK(vm.stringify(vm.runSource("var total = 0\nfor x, in [[1], [2], [3]]:\n    total = total + x\ntotal\n")) == "6");
        // a count mismatch throws (not a silent whole-list bind)
        CHECK(!ok(vm, "var a, = [1, 2]\n"));
        CHECK(!ok(vm, "for x, in [[1, 2]]:\n    pass\n"));
        // the bare form (no `var`) already worked and still does
        CHECK(vm.stringify(vm.runSource("var xs = [9]\nvar b = 0\nb, = xs\nb\n")) == "9");
    }

    // ===== iter() builtin + the _iter_/_next_-only protocol (the eager _iter_-returns-a-List shim was
    // purged). iter(x) is a re-iterable Iterator view over any iterable; a class is iterable only by
    // returning an iterator (iter(collection) / a native iterator / a _next_ object) from _iter_. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
assert List(iter([1, 2, 3])) == [1, 2, 3]
assert List(iter("ab")) == ["a", "b"]
assert List(iter(range(3))) == [0, 1, 2]
assert sum(iter([1, 2, 3, 4])) == 10
assert type(iter([1])) == "iterator"
# re-iterable like map/filter
var it = iter([1, 2, 3])
assert List(it) == [1, 2, 3] and List(it) == [1, 2, 3]
# _iter_ must return an iterator
class WrapList:
    var _init_ = Function(self): self.xs = [4, 5, 6]
    var _iter_ = Function(self): return iter(self.xs)
assert List(WrapList()) == [4, 5, 6]
)"));
        // a bare List / scalar / self from _iter_ is rejected with a clear message
        CHECK(has(errmsg(vm, "class B:\n    var _iter_ = Function(self): return [1, 2]\ndiscard List(B())\n"),
                  "must return an iterator"));
        CHECK(has(errmsg(vm, "class B:\n    var _iter_ = Function(self): return 42\ndiscard List(B())\n"),
                  "must return an iterator"));
        CHECK(has(errmsg(vm, "class B:\n    var _iter_ = Function(self): return self\ndiscard List(B())\n"),
                  "must return an iterator"));
        // a genuinely cyclic iterator is depth-guarded (catchable), never a native stack overflow
        CHECK(has(errmsg(vm, "class C:\n    var _iter_ = Function(self): return iter(self)\ndiscard List(C())\n"),
                  "recurses too deeply"));
        // iter() of a non-iterable surfaces a clean error on consumption
        CHECK(!ok(vm, "discard List(iter(42))\n"));
    }

    return RUN_TESTS();
}
