// Slot-addressed locals (v1.9): a function's non-captured body locals are addressed by a frame slot
// instead of a name lookup. These tests pin the behavior-PRESERVING contract — closures, capture,
// shadowing, read-before-assign, params/defaults, try/catch/with/unpack, recursion, and methods all
// behave exactly as before — plus the numeric fast path and constant-dedup semantics.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Fresh VM per program so module-scope bindings don't leak between cases.
static std::string run(const std::string& src) {
    KiritoVM vm;
    try { return vm.stringify(vm.runSource(src)); }
    catch (const std::exception& e) { return std::string("THREW: ") + e.what(); }
}
static bool throws(const std::string& src) {
    KiritoVM vm;
    try { vm.runSource(src); return false; } catch (...) { return true; }
}

int main() {
    // --- basic slotting + the loop-counter win case ---
    CHECK(run("var f = Function():\n var a = 1\n var b = 2\n return a + b\nf()") == "3");
    CHECK(run("var f = Function(n):\n var s = 0\n var i = 0\n while i < n:\n  s = s + i\n  i = i + 1\n"
              " return s\nf(100)") == "4950");
    CHECK(run("var f = Function(xs):\n var t = 0\n for x in xs:\n  t = t + x\n return t\nf([3,4,5])") == "12");

    // --- closures: a captured local stays name-based and is shared/mutated across calls ---
    CHECK(run("var mk = Function():\n var n = 0\n var inc = Function():\n  n = n + 1\n  return n\n"
              " return inc\nvar c = mk()\n[c(), c(), c()]") == "[1, 2, 3]");
    // capture only reads (still must see the live value)
    CHECK(run("var mk = Function():\n var x = 10\n var g = Function(): return x\n x = 20\n return g()\nmk()") == "20");

    // --- inner shadows an outer local: the outer stays slotted and correct ---
    CHECK(run("var f = Function():\n var x = 1\n var g = Function():\n  var x = 99\n  return x\n"
              " return [x, g()]\nf()") == "[1, 99]");

    // --- capture two levels deep: a grandchild mutating the outer local forces it name-based ---
    CHECK(run("var outer = Function():\n var x = 1\n var mid = Function():\n  var inner = Function():\n"
              "   x = x + 10\n   return x\n  return inner()\n var r = mid()\n return [r, x]\nouter()") == "[11, 11]");

    // --- read/rebind-before-assign is STRICT (no fallback): a bare `=` to a local before its own
    //     `var` runs is a "not defined" error, NOT a walk out to an enclosing binding of the same
    //     name. (The former find-outer-rebind, `[7, 5]`, is gone under strict lexical addressing.) ---
    CHECK(throws("var x = 100\nvar f = Function():\n x = 5\n var x = 7\n return x\nf()"));
    // read-before-assign WITHOUT any binding -> NameError (UnboundLocal-style), like before
    CHECK(throws("var f = Function():\n var y = z\n var z = 1\n return y\nf()"));

    // --- parameters stay name-based: defaults (fresh per call) + captured params still work ---
    CHECK(run("var f = Function(a, b = 10):\n var g = Function(): return a + b\n return g()\nvar r = f(a=2)\nr") == "12");
    CHECK(run("var f = Function(a, b = 10):\n var g = Function(): return a + b\n return g()\nvar r = f(2, 5)\nr") == "7");
    CHECK(run("var f = Function(xs = []):\n xs.append(1)\n return len(xs)\nvar a = f()\nvar b = f()\n[a, b]") == "[1, 1]");  // not shared
    CHECK(run("var f = Function(a: Integer, b: Integer):\n var c = a * b\n return c\nf(b=3, a=4)") == "12");

    // --- try/catch var, with-as, unpacking (+ starred), swap — all slotted store targets ---
    CHECK(run("var f = Function():\n try:\n  throw \"boom\"\n catch String as e:\n  return e\nf()") == "boom");
    CHECK(run("var f = Function():\n var a, b, c = [1, 2, 3]\n a, b = b, a\n var first, *rest = [10,20,30,40]\n"
              " return [a, b, c, first, rest]\nf()") == "[2, 1, 3, 10, [20, 30, 40]]");
    CHECK(run("var d = {\"a\": 1, \"b\": 2}\nvar f = Function(m):\n var t = 0\n for k, v in m.items():\n"
              "  t = t + v\n return t\nf(d)") == "3");

    // --- recursion / mutual recursion: a function's own name lives in the enclosing scope ---
    CHECK(run("var fib = Function(n): return n if n < 2 else fib(n-1) + fib(n-2)\nfib(10)") == "55");
    CHECK(run("var ev = Function(n): return True if n == 0 else od(n-1)\n"
              "var od = Function(n): return False if n == 0 else ev(n-1)\nev(10)") == "True");
    // the call-depth guard still fires on runaway recursion (catchable, not a crash)
    CHECK(throws("var r = Function(n): return r(n + 1)\nr(0)"));

    // --- with-as: the manager (a hidden $with slot) and the bound name both work ---
    CHECK(run("class Ctx:\n var _enter_ = Function(self): return 42\n var _exit_ = Function(self): return None\n"
              "var f = Function():\n with Ctx() as v:\n  return v\nf()") == "42");

    // --- method bodies are slotted (self is a name-based param); inheritance + _super_ intact ---
    CHECK(run("class Animal:\n var _init_ = Function(self, name):\n  self.name = name\n"
              " var speak = Function(self): return self.name + \" sound\"\n"
              "class Dog(Animal):\n var speak = Function(self):\n  var prefix = \"woof: \"\n"
              "  return prefix + self._super_().speak()\n"
              "var d = Dog(\"Rex\")\nd.speak()") == "woof: Rex sound");
    // a class defined INSIDE a function: its name is bound in its frame slot, like `var P = ...`
    CHECK(run("var mk = Function():\n class P:\n  var _init_ = Function(self, x):\n   self.x = x\n"
              " return P(5).x\nmk()") == "5");

    // --- numeric fast path: identical semantics (exact float ==, wraparound, Py3 division, seq *) ---
    CHECK(run("0.1 + 0.2 == 0.3") == "False");
    CHECK(run("3 * \"ab\"") == "ababab");
    CHECK(run("\"ab\" * 3") == "ababab");
    CHECK(run("[7 // 2, 7 % 2, 2 ** 10, 7 / 2]") == "[3, 1, 1024, 3.5]");
    CHECK(run("9223372036854775807 + 1") == "-9223372036854775808");  // two's-complement wraparound
    CHECK(throws("1 / 0"));                                            // zero divisor still throws
    CHECK(run("[1 < 2, 2.0 <= 2.0, 3 > 5, 1 == 1.0]") == "[True, True, False, True]");

    // --- constant dedup is transparent: repeated literals still produce correct values ---
    CHECK(run("var f = Function():\n var a = \"x\"\n var b = \"x\"\n return [a, b, 1, 1, 1]\nf()")
          == "['x', 'x', 1, 1, 1]");

    return RUN_TESTS();
}
