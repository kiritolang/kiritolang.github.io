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

    // === A07-1 (Medium, uncatchable crash): `_iter_` returning `self` re-dispatched iterate() into
    // unbounded native recursion (the balanced _iter_ call kept the depth guard from tripping) → SIGSEGV.
    // Now bounded → a catchable error. ===
    CHECK(has(err("class C:\n    var _iter_ = Function(self): return self\nfor x in C():\n    pass"),
              "recurses too deeply"));
    // a mutually-referential _iter_ pair likewise terminates with a clean error, not a crash
    CHECK(has(err("class A:\n    var other = None\n    var _iter_ = Function(self): return self.other\n"
                  "var a = A()\nvar b = A()\na.other = b\nb.other = a\nfor x in a:\n    pass"),
              "recurses too deeply"));
    // a legitimate _iter_ chain (instance -> List) still works
    CHECK(ok("class R:\n    var _iter_ = Function(self): return [1, 2, 3]\nvar t = 0\nfor x in R():\n    t = t + x\nt") == "6");

    // === A18-1 (Medium, security — CRLF header injection): user headers/cookies were written verbatim
    // into the request; a CR/LF splits/smuggles headers. Now rejected before any connection. ===
    // (\\r\\n in the C++ literal -> Kirito \r\n escapes -> CR/LF bytes in the header value)
    CHECK(has(err("var n = import(\"net\")\nn.get(\"http://127.0.0.1:9/\", {\"headers\": {\"X\": \"a\\r\\nEvil: 1\"}})"),
              "control character"));
    CHECK(has(err("var n = import(\"net\")\nn.get(\"http://127.0.0.1:9/\", {\"cookies\": {\"s\": \"v\\r\\nInjected: 1\"}})"),
              "control character"));

    // === A13-1 (Medium): fmod is a math domain error for an infinite dividend (libm returns silent NaN),
    // consistent with the divisor-zero guard and the module-wide throw-don't-return-NaN policy. ===
    CHECK(has(err("var m = import(\"math\")\nm.fmod(m.inf, 3.0)"), "domain error"));
    CHECK(ok("var m = import(\"math\")\nString(m.fmod(5.0, 3.0))") == "2.0");        // normal
    CHECK(ok("var m = import(\"math\")\nString(m.fmod(5.0, m.inf))") == "5.0");      // finite / inf = finite

    // === A16-1 (Medium, GC-safety): Matrix.apply passed an un-rooted makeFloat arg across the callback;
    // a GC during call() could sweep it. Rooted now — round-trips under per-alloc GC. ===
    CHECK(okGc1("var mx = import(\"matrix\")\nvar a = mx.identity(3)\n"
                "var b = a.apply(Function(x): return x + 1.0)\nString(b[0, 0])") == "2.0");

    // === A15-1 (Medium-high, autograd): a retained non-leaf tensor's `.grad` was used as the
    // reverse-mode accumulator and never cleared between passes, so a second backward() through it
    // re-seeded the stale value and doubled the leaf gradient. Non-leaves are now grad-reset each
    // pass (leaves keep accumulating, as the tests rely on). ===
    {
        const std::string pre = "var T = import(\"tensor\")\n";
        // Two passes through the SAME intermediate y = x*x; x.grad must be [2,4], not [4,8].
        CHECK(ok(pre + "var x = T.Tensor([1.0, 2.0], requiresgrad = True)\nvar y = x * x\n"
                       "y.sum().backward()\nx.zerograd()\ny.sum().backward()\nString(x.grad.tolist())")
              == "[2.0, 4.0]");
        // Driving an intermediate root directly twice: leaf grad is exactly 2x a single pass (linear).
        CHECK(ok(pre + "var x = T.Tensor([1.0, 2.0, 3.0], requiresgrad = True)\nvar y = x * x\n"
                       "y.backward(T.ones([3]))\ny.backward(T.ones([3]))\nString(x.grad.tolist())")
              == "[4.0, 8.0, 12.0]");  // each pass contributes 2x = [2,4,6]; two passes -> [4,8,12]
        // A single pass still gives the un-doubled result (no regression).
        CHECK(ok(pre + "var x = T.Tensor([1.0, 2.0], requiresgrad = True)\n(x * x).sum().backward()\n"
                       "String(x.grad.tolist())") == "[2.0, 4.0]");
    }

    // === A18-3 (Low, use-after-close): detach() left `fd` intact, so a second detach handed out the
    // same fd (double-adopt) and fileno() after close/detach returned a stale/recycled number. Now
    // detach resets fd (second detach throws) and fileno returns -1 on a closed socket. ===
    {
        const std::string pre = "var net = import(\"net\")\n";
        CHECK(has(err(pre + "var s = net.tcpsocket()\ndiscard s.detach()\ndiscard s.detach()"),
                  "socket is closed"));                                    // detach-twice throws
        CHECK(ok(pre + "var s = net.tcpsocket()\ns.close()\nString(s.fileno())") == "-1");  // Python-style
        CHECK(ok(pre + "var s = net.tcpsocket()\ndiscard s.detach()\nString(s.fileno())") == "-1");
        CHECK(ok(pre + "var s = net.tcpsocket()\nString(s.fileno() >= 0)") == "True");       // open: real fd
    }

    // === A18-4 (Medium, hang): settimeout() now also bounds connect() (it stores the value and connect
    // uses non-blocking connect + select). Deterministic assertion: settimeout is accepted and a connect
    // to a refused local port still fails promptly (connection refused), i.e. the timeout path doesn't
    // break normal error reporting. The true black-hole-timeout case is environment-sensitive. ===
    {
        const std::string pre = "var net = import(\"net\")\n";
        CHECK(has(err(pre + "var s = net.tcpsocket()\ns.settimeout(2.0)\ns.connect(\"127.0.0.1\", 9)"),
                  "connect failed"));   // refused (or timed out) — either way a clean throw, no hang
    }

    return RUN_TESTS();
}
