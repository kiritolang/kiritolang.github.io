// Regression tests for the v1.13 audit (.audit/v1.13/). Each block pins one confirmed finding so it
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
    // A class whose _iter_ returns iter() over a freshly-built List: the returned iterator is dropped,
    // so its elements are reachable only via the (was-unrooted) cursor items.
    CHECK(okGc1("class R:\n    var _iter_ = Function(self): return iter([10, 20, 30])\n"
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

    // === A07-1 (Medium, uncatchable crash → clean error): `_iter_` must return an ITERATOR, so a bare
    // `self`/`self.other` (no _next_) is rejected outright — no re-dispatch, no recursion. The genuinely
    // cyclic form `return iter(self)` (a valid native iterator that re-iterates self) is bounded by the
    // SourceCursor native-nesting guard → a catchable error, never SIGSEGV. ===
    CHECK(has(err("class C:\n    var _iter_ = Function(self): return self\nfor x in C():\n    pass"),
              "must return an iterator"));
    // a mutually-referential _iter_ pair returns bare instances (no _next_) → also rejected outright
    CHECK(has(err("class A:\n    var other = None\n    var _iter_ = Function(self): return self.other\n"
                  "var a = A()\nvar b = A()\na.other = b\nb.other = a\nfor x in a:\n    pass"),
              "must return an iterator"));
    // the genuinely cyclic iterator (return iter(self)) is depth-guarded, not a crash
    CHECK(has(err("class C:\n    var _iter_ = Function(self): return iter(self)\nfor x in C():\n    pass"),
              "recurses too deeply"));
    // a legitimate _iter_ over a wrapped List still works
    CHECK(ok("class R:\n    var _iter_ = Function(self): return iter([1, 2, 3])\nvar t = 0\nfor x in R():\n    t = t + x\nt") == "6");

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

    // === A24-1 (statistics.mean int64 overflow): Float(sum(data)) summed all-Integer data in int64
    // FIRST, so a large-integer dataset wrapped (two's-complement) before the Float conversion. mean
    // now accumulates in Float space. Two copies of 2^62 sum to exactly INT64_MAX+1 -> the old code
    // wrapped to a negative mean; the fix yields ~4.6e18. ===
    CHECK(ok("var st = import(\"statistics\")\n"
             "String(st.mean([4611686018427387904, 4611686018427387904]) > 0.0)") == "True");
    CHECK(ok("var st = import(\"statistics\")\nString(st.mean([2, 4, 6]))") == "4.0");  // no regression

    // === A14-1 (random.gauss keyword-only): makeMethod hole-fills a skipped leading optional (mu)
    // with None when a LATER arg (sigma) is passed by keyword, and the impl fed None to asNum -> throw.
    // A None slot now means "use the default". ===
    CHECK(ok("var r = import(\"random\").Random(1)\nString(r.gauss(sigma = 2.0) == r.gauss(sigma = 2.0))")
          == "False");                                    // runs (draws two values), doesn't throw
    CHECK(has(err("var r = import(\"random\").Random(1)\ndiscard r.gauss(sigma = -1.0)"), "sigma"));  // A14-3

    // === A21-2 (regex endpos hole-fill): match/search/finditer/findall with `endpos=` but `pos`
    // omitted hole-filled pos with None, then asInt(None) threw. None slots are now "absent". ===
    CHECK(ok("var re = import(\"regex\")\nre.compile(\"a+\").search(\"aaa\", endpos = 2).group()") == "aa");
    CHECK(ok("var re = import(\"regex\")\nString(len(re.compile(\"a\").findall(\"aaaa\", endpos = 2)))") == "2");
    // A21-3: a huge pos/endpos is clamped, not misbehaving (past-end pos matches nothing, no OOB).
    CHECK(ok("var re = import(\"regex\")\nString(re.compile(\"a\").search(\"aaa\", 999999999999) == None)") == "True");

    // === A09-2 (NaN sort UB): the sort/min/max comparator returned false both ways for a NaN operand,
    // making NaN equivalent to every number (not a strict weak ordering -> std::sort UB). NaN now sorts
    // as the largest value. Assert a stable, total order (no crash under ASan, deterministic result). ===
    CHECK(ok("var m = import(\"math\")\nvar s = sorted([3.0, m.nan, 1.0, 2.0, m.nan])\n"
             "String(s[0]) + \",\" + String(s[1]) + \",\" + String(s[2]) + \",\" + String(s[3] != s[3]) + \",\" + String(s[4] != s[4])")
          == "1.0,2.0,3.0,True,True");                    // reals first (1,2,3), the two NaNs last
    CHECK(ok("var m = import(\"math\")\nString(min([2.0, 1.0, 3.0]))") == "1.0");   // no regression
    CHECK(ok("var m = import(\"math\")\nString(max([2.0, 1.0, 3.0]))") == "3.0");

    // === A17-1 (serde wrong-bucket): a Set element / Dict key with a CONTENT-based hash (Bytes,
    // DateTime, attr-based _hash_) was wired into the Set/Dict in pass 2 — BEFORE its payload was
    // restored by _setstate_ (pass 3) — so it bucketed under its empty hash, and a later lookup on the
    // real hash landed in a different bucket (silent membership corruption). Set/Dict wiring is now a
    // final pass after all state is restored. ===
    {
        const std::string dl = "var d = import(\"dump\")\n";     // dump (binary)
        const std::string sl = "var d = import(\"serialize\")\n";// serialize (text) — shared core
        for (const std::string& pre : {dl, sl}) {
            // Set of Bytes: both members found after round-trip.
            CHECK(ok(pre + "var s = {Bytes([1, 2]), Bytes([3, 4])}\nvar b = d.loads(d.dumps(s))\n"
                          "String(b.contains(Bytes([1, 2]))) + String(b.contains(Bytes([3, 4]))) + String(len(b))")
                  == "TrueTrue2");
            // Dict keyed by Bytes: key lookup succeeds.
            CHECK(ok(pre + "var m = {Bytes([9]): \"nine\"}\nvar b = d.loads(d.dumps(m))\nb[Bytes([9])]") == "nine");
        }
        // Dict/Set keyed by DateTime (hash by epoch; empty epoch=0 at insert without the fix).
        CHECK(ok("var d = import(\"dump\")\nvar t = import(\"time\")\nvar dt = t.make(2020, 1, 1)\n"
                 "var b = d.loads(d.dumps({dt: \"y\"}))\nb[dt]") == "y");
        // A user class with an attribute-based _hash_ used as a Set member.
        CHECK(ok("var d = import(\"dump\")\n"
                 "class K:\n    var _init_ = Function(self, v): self.v = v\n"
                 "    var _hash_ = Function(self): return self.v\n"
                 "    var _eq_ = Function(self, o): return self.v == o.v\n"
                 "var b = d.loads(d.dumps({K(5), K(7)}))\nString(b.contains(K(5))) + String(b.contains(K(7)))")
              == "TrueTrue");
    }

    // === A04-2 / A04-3 (switch negative case labels): a negative label `-1` parses as UnaryExpr(Neg,
    // Literal), not a LiteralExpr, so it (a) escaped duplicate detection — `case -1` twice was accepted
    // — and (b) dropped the whole switch off the O(1) dispatch. The compiler now constant-FOLDS every
    // case label (foldConstValue, via the VM's own operators), so a negated literal is a proper constant:
    // duplicates are a compile error and negative cases dispatch correctly. ===
    CHECK(has(err("var x = 5\nswitch x:\n    case -1:\n        pass\n    case -1:\n        pass"),
              "duplicate switch case value"));
    CHECK(has(err("var x = 5\nswitch x:\n    case -1.5:\n        pass\n    case -1.5:\n        pass"),
              "duplicate switch case value"));
    // a negative and its positive are NOT duplicates
    CHECK(ok("var x = 5\nswitch x:\n    case -1:\n        pass\n    case 1:\n        pass\n1") == "1");
    // negative-case dispatch returns the right arm (fast path)
    CHECK(ok("var f = Function(x):\n    switch x:\n        case -1:\n            return \"a\"\n"
             "        case -2:\n            return \"b\"\n        default:\n            return \"c\"\n"
             "f(-1) + f(-2) + f(-9)") == "abc");

    // === A19-4 (env thread-safety): the four env entry points are now serialized by a process-wide
    // mutex (the tsan build is the real race gate). Functional no-regression: set/get/unset round-trip. ===
    CHECK(ok("var sys = import(\"sys\")\nsys.setenv(\"KI_AUDIT_V113\", \"x\")\nsys.getenv(\"KI_AUDIT_V113\")") == "x");
    CHECK(ok("var sys = import(\"sys\")\nsys.setenv(\"KI_AUDIT_V113\", \"y\")\nsys.unsetenv(\"KI_AUDIT_V113\")\n"
             "sys.getenv(\"KI_AUDIT_V113\", \"gone\")") == "gone");

    // === A19-1 (DateTime epoch year-narrow): gmtimeCompat computed the true year as int64 then
    // narrowed to int BEFORE setEpoch's range check read it, so a pathological epoch wrapped into the
    // valid [-9999, 9999] band and a corrupt DateTime was accepted. The range is now validated on the
    // pre-narrow int64 year. ===
    CHECK(has(err("var t = import(\"time\")\ndiscard t.datetime(999999999999999999)"), "out of representable range"));
    CHECK(has(err("var t = import(\"time\")\ndiscard t.datetime(-999999999999999999)"), "out of representable range"));
    CHECK(ok("var t = import(\"time\")\nt.datetime(0).iso()") == "1970-01-01T00:00:00");  // sane epoch intact

    // === A11-1 (round ndigits): on this platform long double has extra precision, so the extended-
    // scaling path is unchanged — pin the documented half-away-from-zero result so the #if guard around
    // the MSVC fallback can't silently regress it. ===
    CHECK(ok("String(round(2.675, 2))") == "2.67");   // extended precision avoids the 2.675*100 double-round
    CHECK(ok("String(round(2.5))") == "3");           // half away from zero
    CHECK(ok("String(round(-2.5))") == "-3");
    CHECK(ok("String(round(1.2345, 3))") == "1.234");

    // === A01-1/A01-2/A01-3 (f-string escape decoder DRY): the f-string literal-part decoder diverged
    // from the plain-string lexer — `\xHH` emitted a RAW byte (breaking the code-point invariant, so
    // f"\xff" != "\xff") and an unknown escape was silently dropped (f"\q" -> "q") instead of the
    // documented lex error. Both now route through the shared common.hpp decodeCookedEscape. ===
    CHECK(ok("String(f\"\\xff\" == \"\\xff\")") == "True");   // \xHH is a code point (UTF-8), not a raw byte
    CHECK(ok("String(len(f\"\\xe9\"))") == "1");              // one code point, not one/two raw bytes
    CHECK(ok("String(f\"\\xe9\" == \"é\")") == "True");
    CHECK(ok("String(f\"a\\tb\" == \"a\\tb\")") == "True");   // ordinary escapes unchanged
    CHECK(has(err("var s = f\"a\\qb\""), "invalid escape"));  // unknown escape now throws (was silent)
    CHECK(has(err("var s = f\"\\x4\""), "invalid \\x escape"));  // short \x throws in f-strings too
    // plain-string behaviour is unchanged (same shared decoder)
    CHECK(ok("String(\"\\xe9\" == \"é\")") == "True");
    CHECK(has(err("var s = \"a\\qb\""), "invalid escape"));

    // === A02-1 (inline-function comma-pack): `var f = Function(): return a, b` silently absorbed the
    // `, b` into a top-level pack -> f bound to [<function>, b], function returning only `a`. Inline
    // functions don't pack; the ambiguous comma is now rejected. Call-arg / bracketed-list commas
    // (which delimit) still work. ===
    CHECK(has(err("var a = 1\nvar b = 2\nvar f = Function(): return a, b"), "inline function cannot be comma-packed"));
    CHECK(has(err("var t = Function(): pass, 1"), "inline function cannot be comma-packed"));
    CHECK(has(err("var x = 0\nvar t = 1, Function(): return x, 2"), "inline function cannot be comma-packed"));
    // an inline function as a call argument followed by more args is fine (comma delimits args)
    CHECK(ok("String(List(map(Function(x): return x * 2, [1, 2, 3])))") == "[2, 4, 6]");
    CHECK(ok("String(sorted([3, 1, 2], key = Function(x): return -x))") == "[3, 2, 1]");
    // an inline function inside a List literal followed by more elements is fine (brackets delimit)
    CHECK(ok("var xs = [Function(): return 9, 1]\nString(len(xs)) + String(xs[0]()) + String(xs[1])") == "291");
    // a BLOCK-bodied function still packs its return normally
    CHECK(ok("var f = Function():\n    return 1, 2\nString(f())") == "[1, 2]");
    // an inline function with no trailing comma is unaffected
    CHECK(ok("var f = Function(): return 42\nString(f())") == "42");

    return RUN_TESTS();
}
