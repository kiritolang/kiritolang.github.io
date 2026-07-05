// Regression tests for the v1.13 audit (audit_v1_13/). Each block pins one confirmed finding so it
// can never silently return. Several are memory-safety bugs — run under -fsanitize=address,undefined:
// the sanitizer build is the real gate for A05-1 (GC use-after-free) and A09-1 (List-search UAF).
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string err(const std::string& src) {
    KiritoVM vm;
    vm.installStandardLibrary();
    try { vm.runSource(src); return ""; }
    catch (const KiritoError& e) { return e.what(); }
    catch (const std::exception& e) { return std::string("std:") + e.what(); }
}
static std::string ok(const std::string& src) {
    KiritoVM vm;
    vm.installStandardLibrary();
    return vm.stringify(vm.runSource(src));
}
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
// Run `src` collecting on EVERY allocation, so an unrooted intermediate is swept immediately
// (release: throws "dangling handle"; ASan: heap-use-after-free) — the deterministic GC-root gate.
static std::string okGc1(const std::string& src) {
    KiritoVM vm;
    vm.installStandardLibrary();
    vm.setGcThreshold(1);
    return vm.stringify(vm.runSource(src));
}

int main() {
    // === A05-1 (HIGH, GC use-after-free): Op::GetIter's eager branch rooted freshly-materialised
    // iterator items in a RootScope scoped to the `else` block — destroyed BEFORE alloc(cursor), which
    // runs a GC before inserting the cursor. So `for c in "abc"` swept the fresh 1-char String handles
    // (ASan UAF / release dangling handle). The RootScope is now hoisted across the alloc. ===
    CHECK(okGc1("var s = \"\"\nfor c in \"abc\":\n    s = s + c\ns") == "abc");
    // Bytes iterate() allocates fresh per-byte Integers (not the Bytes' children()) — same gap.
    CHECK(okGc1("var xs = []\nfor b in Bytes([65, 66, 67]):\n    xs.append(b)\nlen(xs)") == "3");
    // A class whose _iter_ returns a freshly-built List: the returned iterable is dropped, so its
    // elements are reachable only via the (was-unrooted) cursor items.
    CHECK(okGc1("class R:\n    var _iter_ = Function(self): return [10, 20, 30]\n"
                "var t = 0\nfor x in R():\n    t = t + x\nt") == "60");
    // non-empty String with multi-byte code points (fresh handles per code point)
    CHECK(okGc1("var n = 0\nfor c in \"héllo\":\n    n = n + 1\nn") == "5");

    // === A03-1 (HIGH, capture analysis): a nested function's parameter default is compiled to its own
    // Proto and evaluated at call time through ITS closure, so a reference there to an enclosing
    // function's local is a genuine capture. It was frame-slotted, so the default resolved a wrong
    // outer binding (silent 105) or threw a spurious NameError. Both forms must now be 15. ===
    CHECK(ok("var z = 100\nvar make = Function():\n    var z = 10\n"
             "    var g = Function(x, y = z): return x + y\n    return g\nmake()(5)") == "15");
    // no outer shadow -> the bug threw `name 'z' is not defined` at call time; must be 15.
    CHECK(ok("var make = Function():\n    var z = 10\n"
             "    var g = Function(x, y = z): return x + y\n    return g\nmake()(5)") == "15");
    // The default referencing an EARLIER nested param must still bind to the param, not capture.
    CHECK(ok("var f = Function(a, b = a): return a + b\nf(7)") == "14");
    // Two-level nesting + a default capturing the outer-outer local.
    CHECK(ok("var mk = Function():\n    var base = 3\n"
             "    var outer = Function():\n        var inner = Function(x, y = base): return x + y\n"
             "        return inner\n    return outer\nmk()()(4)") == "7");

    // === A04-1 (HIGH, operand-stack corruption): `return EXPR` crossing a finally that does
    // break/continue popped the return value instead of the loop cursor, orphaning it — an enclosing
    // loop's ForIter then read the leaked cursor (observed: infinite hang / garbage). The return value
    // is now parked in a hidden local across the crossed cleanups. ===
    CHECK(ok("var f = Function():\n    var out = []\n    for i in [1, 2]:\n"
             "        for j in [10, 20, 30]:\n            try:\n                return 99\n"
             "            finally:\n                break\n        out.append(i)\n    return out\nf()") == "[1, 2]");
    // continue-in-finally abandons each return; after the loop, `return n` (n stays 0).
    CHECK(ok("var f = Function():\n    var n = 0\n    for i in [1, 2, 3]:\n"
             "        try:\n            return 99\n        finally:\n            continue\n"
             "    return n\nf()") == "0");
    // A normal finally (no break/continue) must still let the return actually return its value.
    CHECK(ok("var f = Function():\n    for i in [1]:\n        try:\n            return 42\n"
             "        finally:\n            pass\n    return -1\nf()") == "42");
    // return crossing a `with` whose _exit_ runs, under a loop — value must survive.
    CHECK(ok("class Ctx:\n    var _enter_ = Function(self): return self\n"
             "    var _exit_ = Function(self): return None\n"
             "var f = Function():\n    for i in [1]:\n        with Ctx() as c:\n            return 7\n    return -1\nf()") == "7");

    // === A09-1 (HIGH, List-search UAF): remove/index/count/in searched over a live `elems` reference
    // while kiEquals could run a user _eq_ that reallocs the list. They now search a GC-rooted
    // snapshot. Adversarial: a hostile _eq_ (on the SEARCHED value) mutates the list mid-search —
    // must not crash (ASan gate), and returns a sane result. ===
    const std::string evil =
        "class Evil:\n    var _eq_ = Function(self, o):\n        xs.append(0)\n        return False\n"
        "var xs = [1, 2, 3, 4, 5, 6, 7, 8]\n";
    CHECK(has(err(evil + "xs.remove(Evil())"), "not in List"));  // no match -> clean throw, no UAF
    CHECK(has(err(evil + "xs.index(Evil())"), "not in List"));
    CHECK(ok(evil + "xs.count(Evil())") == "0");
    CHECK(ok(evil + "String(Evil() in xs)") == "False");
    // Positive: the value-search methods still behave correctly with no reentrancy.
    CHECK(ok("var xs = [1, 2, 3, 2]\nxs.remove(2)\nxs") == "[1, 3, 2]");
    CHECK(ok("[1, 2, 3, 2].index(2)") == "1");
    CHECK(ok("[1, 2, 3, 2].count(2)") == "2");
    CHECK(ok("String(3 in [1, 2, 3]) + String(9 in [1, 2, 3])") == "TrueFalse");
    // index with a start/end window still works after the snapshot rework.
    CHECK(ok("[1, 2, 3, 2, 1].index(2, 2)") == "3");

    return RUN_TESTS();
}
