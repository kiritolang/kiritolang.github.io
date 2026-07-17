// Regression tests for the v1.12 hardening pass (.audit/v1.12/FINDINGS.md). Each block pins one
// confirmed finding so it can never silently return. Run under -fsanitize=address,undefined — several
// of these were UBSan/ASan-confirmed crashes, so the sanitizer build is the real gate.
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
// The source line a KiritoError points at (0 if none / no error) — for pinning diagnostic locations.
static uint32_t errLine(const std::string& src) {
    KiritoVM vm;
    vm.installStandardLibrary();
    try { vm.runSource(src); return 0; }
    catch (const KiritoError& e) { return e.span.line; }
    catch (...) { return 0; }
}
// Run `src` collecting on EVERY allocation, so an unrooted intermediate is swept immediately
// (release: throws "dangling handle"; ASan: heap-use-after-free) — the deterministic T-ROOT gate.
static std::string okGc1(const std::string& src) {
    KiritoVM vm;
    vm.installStandardLibrary();
    vm.setGcThreshold(1);
    return vm.stringify(vm.runSource(src));
}

int main() {
    // === A07-1: InstanceValue::equals must not downcast a NATIVE value that also reports
    // ValueKind::Instance. `Bytes("") in [C()]` makes List::contains call C-instance.equals(Bytes)
    // (List uses raw Object::equals, no hashing) — previously a static_cast<InstanceValue&>(Bytes)
    // read a garbage Handle (UBSan-confirmed). Must return False cleanly, no crash. ===
    CHECK(ok("class C:\n  var _eq_ = Function(self, o): return False\n"
             "Bytes(\"\") in [C()]") == "False");
    // A user instance and a native Bytes coexisting as Dict keys must not corrupt the container.
    CHECK(ok("class C:\n  var _hash_ = Function(self): return 0\n"
             "  var _eq_ = Function(self, o): return False\n"
             "var d = {}\nd[C()] = 1\nd[Bytes(\"\")] = 2\nlen(d)") == "2");
    // The List path is the DETERMINISTIC trigger for InstanceValue::equals(Bytes): List.contains does a
    // linear scan calling element.equals(query) with no hashing, so `Bytes("") in [C()]` (above) always
    // exercises the fixed dynamic_cast. A native ValueKind::Instance (DateTime) vs a user instance also
    // must not misfire the downcast — coexist as Set members cleanly.
    CHECK(ok("var tm = import(\"time\")\nclass C:\n  var _hash_ = Function(self): return 7\n"
             "  var _eq_ = Function(self, o): return False\n"
             "len({C(), tm.make(2020, 1, 1)})") == "2");

    // === A09-1: modular pow must not overflow int64 during `(base % mod) + mod`. With mod just under
    // 2^63, base ≡ -1, so base^2 ≡ 1 — the buggy int64 path returned 9. ===
    CHECK(ok("pow(9223372036854775806, 2, 9223372036854775807)") == "1");
    CHECK(ok("pow(9223372036854775805, 2, 9223372036854775807)") == "4");  // (-2)^2 = 4
    CHECK(ok("pow(7, 0, 9223372036854775807)") == "1");
    CHECK(has(err("pow(2, 10, 0)"), "modulus"));       // guards still fire
    CHECK(has(err("pow(2, -1, 5)"), "non-negative"));

    // === M1 (perf): the DEFAULT (adaptive) GC threshold still reclaims. 300k short-lived allocations
    // with no explicit setGcThreshold must keep the live set small (collections run + retarget). ===
    {
        KiritoVM vm;
        vm.installStandardLibrary();
        vm.runSource("var i = 0\nwhile i < 300000:\n  var t = [i, i + 1]\n  i = i + 1\n");
        CHECK(vm.liveCount() < 100000);
    }
    // setGcThreshold still pins exactly (adaptive off) — collect-on-every-alloc stays honored and
    // the built container survives the aggressive GC without a dangling handle.
    {
        KiritoVM vm;
        vm.installStandardLibrary();
        vm.setGcThreshold(1);
        CHECK(vm.stringify(vm.runSource(
            "var xs = []\nvar i = 0\nwhile i < 500:\n  xs.append([i])\n  i = i + 1\nlen(xs)")) == "500");
    }

    // === T-ROOT (A09-3/4 High, A06-1/2 Medium): every iterable-consuming builtin and every
    // apply/sort snapshot roots its fresh element handles across a callback/rehash that allocates.
    // Source is a String so iteration yields FRESH per-char handles (the vulnerable case); under
    // gcThreshold(1) an unrooted element is swept the instant the next allocation happens. ===
    CHECK(okGc1("len(Set(\"abcdefghij\"))") == "10");                              // A09-3 Set ctor
    CHECK(okGc1("len(Dict([[\"a\", 1], [\"b\", 2], [\"c\", 3]]))") == "3");        // A09-3 Dict ctor
    CHECK(okGc1("len(List(\"abcdefghij\"))") == "10");                            // List ctor
    // map/filter/zip/enumerate are lazy now (no len()) — List(...) drains them (drainLazy rooting).
    CHECK(okGc1("len(List(map(Function(c): return c + \"!\", \"abcdefghij\")))") == "10");  // A09-4 map
    CHECK(okGc1("len(List(filter(Function(c): return True, \"abcdefghij\")))") == "10");    // A09-4 filter
    CHECK(okGc1("sorted(\"dbca\")") == "['a', 'b', 'c', 'd']");                    // A09-4 sorted (key/_lt_)
    CHECK(okGc1("all(map(Function(x): return True, \"abcde\"))") == "True");       // A09-4 all (streams)
    CHECK(okGc1("any(map(Function(x): return False, \"abcde\"))") == "False");     // A09-4 any (streams)
    CHECK(okGc1("len(List(zip(\"abc\", \"defgh\")))") == "3");                     // zip columns
    CHECK(okGc1("len(List(enumerate(\"abcde\")))") == "5");                        // enumerate
    CHECK(okGc1("len(reversed(\"abcde\"))") == "5");                               // reversed
    // A06-1/2: apply's fn CLEARS the receiver (dropping the only other reference to the snapshot's
    // fresh string handles) then allocates — the snapshot must stay rooted. Elements are Strings, not
    // interned small ints, so a swept element genuinely dangles.
    CHECK(okGc1("var xs = [\"aa\", \"bb\", \"cc\", \"dd\", \"ee\"]\n"
                "var f = Function(x):\n  xs.clear()\n  return x + \"!\"\n"
                "len(xs.apply(f))") == "5");
    CHECK(okGc1("var s = Set([\"aa\", \"bb\", \"cc\", \"dd\", \"ee\"])\n"
                "var g = Function(x):\n  s.clear()\n  return x + \"!\"\n"
                "len(s.apply(g))") == "5");

    // === A04-1: recursion routed through a native higher-order builtin carries deep C++ frames, so a
    // COUNT-only guard overflows the native stack before firing (SIGSEGV). The stack-usage guard must
    // catch it and throw a catchable "maximum recursion depth exceeded" instead — no crash. ===
    CHECK(has(err("var g = Function(n): return sorted([n], key = g)\ndiscard g(0)"), "recursion"));
    CHECK(has(err("var h = Function(n): return [n].apply(h)\ndiscard h(0)"), "recursion"));
    CHECK(has(err("var m = Function(n): return max([n, n], key = m)\ndiscard m(0)"), "recursion"));
    // Plain (shallow-frame) Kirito recursion still throws catchably via the count guard.
    CHECK(has(err("var f = Function(n): return f(n + 1)\ndiscard f(0)"), "recursion"));
    // And ordinary bounded recursion still works (guard doesn't fire early on legitimate depth).
    CHECK(ok("var fact = Function(n): return 1 if n <= 1 else n * fact(n - 1)\nfact(20)")
          == "2432902008176640000");

    // === TENSOR-1: the 64-dimension rank cap must hold for tensors built by reshape/expanddims/
    // broadcastto (not just the nested-list ctor), or a huge-rank tensor SIGSEGVs the recursive
    // str()/tolist(). Repeated expanddims past rank 64 must throw catchably. ===
    CHECK(has(err("var t = import(\"tensor\")\nvar a = t.zeros([1])\n"
                  "var i = 0\nwhile i < 100:\n  a = a.expanddims(0)\n  i = i + 1\n"), "dimension"));
    CHECK(ok("var t = import(\"tensor\")\nt.zeros([1]).expanddims(0).ndim()") == "2");  // normal still ok

    // === TENSOR-2: einsum's contraction count is the product of ALL label sizes; computing it
    // unchecked overflowed size_t (wrapping to a small count -> a silently truncated result). The guard
    // now catches ONLY that overflow — it does NOT cap the work at 64M elements (that wrongly rejected a
    // normal matmul-shaped contraction, which the native .matmul() runs fine). ===
    CHECK(ok("var t = import(\"tensor\")\nt.einsum(\"ij,jk->ik\", t.eye(3), t.eye(3)).tolist()")
          == "[[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]");  // small einsum works
    // A contraction with 7e7 iterations (> the old 64M element cap) must SUCCEED now, not throw:
    CHECK(ok("var t = import(\"tensor\")\n"
             "var m = t.einsum(\"ij,jk->ik\", t.ones([100, 7000]), t.ones([7000, 100]))\n"
             "[m.shape(), m.tolist()[0][0]]") == "[[100, 100], 7000.0]");
    // A genuinely astronomical contraction whose label product overflows size_t is rejected (each
    // operand still fits the element cap; the DISTINCT-label product 1000^8 = 1e24 > SIZE_MAX):
    CHECK(has(err("var t = import(\"tensor\")\nvar a = t.ones([1000, 1000])\n"
                  "discard t.einsum(\"ij,jk,kl,lm,mn,no,op->ip\", a, a, a, a, a, a, a)"), "overflow"));

    // === A20-1 (kpm-critical): semver comparator with whitespace before the version (">= 1.2.0")
    // must satisfy like ">=1.2.0", not collapse to exact-match. ===
    CHECK(ok("import(\"semver\").satisfies(\"1.5.0\", \">= 1.2.0\")") == "True");
    CHECK(ok("import(\"semver\").satisfies(\"1.0.0\", \">= 1.2.0\")") == "False");
    CHECK(ok("import(\"semver\").satisfies(\"1.5.0\", \"> 1.2.0\")") == "True");
    CHECK(ok("import(\"semver\").satisfies(\"1.5.0\", \">=1.2.0\")") == "True");   // no-space still ok
    CHECK(ok("import(\"semver\").satisfies(\"1.5.0\", \">= 1.2.0 < 2.0.0\")") == "True");   // AND
    CHECK(ok("import(\"semver\").satisfies(\"2.5.0\", \">= 1.2.0 < 2.0.0\")") == "False");

    // === A16-1: multi-member gzip decompress now bounds the AGGREGATE output with a shrinking budget
    // (a small .gz of many members was an unbounded bomb past the per-member cap). A legitimate
    // multi-member stream (cat of two members) must still round-trip. The full >256 MiB bomb
    // rejection is logic-verified — the per-member inflate cap already has coverage and the budget
    // now shrinks across members, so the aggregate can never exceed it. ===
    CHECK(ok("var g = import(\"gzip\")\n"
             "g.decompress(g.compress(\"hello \") + g.compress(\"world!\"))") == "hello world!");

    // === NET-1 (security): a redirect must not forward the Authorization header / cookie jar to a
    // different origin or over an https->http downgrade. The policy is pure, so test it directly. ===
    {
        auto scope = [](const char* from, const char* to) {
            return net::redirectScope(net::parseUrl(from), net::parseUrl(to));
        };
        // same origin -> keep both
        auto s1 = scope("https://api.example.com/a", "https://api.example.com/b");
        CHECK(!s1.dropAuth); CHECK(!s1.dropCookies);
        // different host -> drop both
        auto s2 = scope("https://api.example.com/a", "https://evil.example.net/b");
        CHECK(s2.dropAuth); CHECK(s2.dropCookies);
        // different port, same host -> drop auth (cookies are host-scoped, not port-scoped -> kept)
        auto s3 = scope("http://127.0.0.1:8080/a", "http://127.0.0.1:9090/b");
        CHECK(s3.dropAuth); CHECK(!s3.dropCookies);
        // https -> http downgrade, same host -> drop auth (cookies same-host kept)
        auto s4 = scope("https://api.example.com/a", "http://api.example.com/b");
        CHECK(s4.dropAuth); CHECK(!s4.dropCookies);
        // http -> https upgrade, same host -> keep both (as requests does)
        auto s5 = scope("http://api.example.com/a", "https://api.example.com/b");
        CHECK(!s5.dropAuth); CHECK(!s5.dropCookies);
    }

    // === A10-4: capturing child stdout/stderr is now bounded (a runaway producer would OOM inside a
    // drain thread with no catch -> SIGABRT). Normal small output must still be captured intact; the
    // >256 MiB truncation-throws path is logic-verified (a fixture producing 256 MiB is impractical). ===
    CHECK(has(ok("import(\"sys\").shell(\"echo hello-kirito\")[\"stdout\"]"), "hello-kirito"));

    // === T-GUARD (A06-8): replace/join cap their amplified output at kMaxRepeat (throw, not OOM the
    // host). Normal use unaffected; the >256 MiB trigger is logic-verified (it would need a ~256 MiB
    // transient). join also now roots its iterated elements (a user _str_ may allocate mid-loop). ===
    CHECK(ok("(\"a\" * 5).replace(\"a\", \"bb\")") == "bbbbbbbbbb");
    CHECK(ok("\",\".join([\"a\", \"b\", \"c\"])") == "a,b,c");
    CHECK(okGc1("\",\".join(\"abcde\")") == "a,b,c,d,e");   // join over a String (fresh handles) under GC1

    // === A05-1: ==/!= are symmetric, so `5 != c` must honor a right-hand instance's standalone `_ne_`
    // (and `_eq_`), instead of falling through to structural equality. (Arithmetic/ordering operators
    // deliberately do NOT reflect — a documented invariant; only ==/!= are symmetric.) ===
    CHECK(ok("class C:\n  var _ne_ = Function(self, o): return True\n5 != C()") == "True");
    CHECK(ok("class C:\n  var _ne_ = Function(self, o): return False\n5 != C()") == "False");
    CHECK(ok("class C:\n  var _eq_ = Function(self, o): return True\n5 == C()") == "True");
    // `!=` derived from a standalone `_eq_` on the RHS (no `_ne_`): 5 != d negates d._eq_(5).
    CHECK(ok("class D:\n  var _eq_ = Function(self, o): return True\n5 != D()") == "False");
    CHECK(ok("class D:\n  var _eq_ = Function(self, o): return False\n5 != D()") == "True");
    // symmetry: `5 == c` and `c == 5` agree; likewise for `!=`.
    CHECK(ok("class C:\n  var _eq_ = Function(self, o): return o == 5\n"
             "[5 == C(), C() == 5, 5 != C(), C() != 5]") == "[True, True, False, False]");
    // and the documented limitation still holds: arithmetic does not reflect onto the right operand.
    CHECK(!err("class C:\n  var _mul_ = Function(self, o): return 1\n3 * C()").empty());  // throws

    // === A05-2 (T-BIND): a keyword call that skips a REQUIRED leading arg must error, not silently
    // gap-fill it with None. `d.setdefault(default=7)` used to insert {None: 7}; now it throws. ===
    CHECK(has(err("var d = {}\nd.setdefault(default = 7)"), "required"));
    CHECK(has(err("var d = {\"a\": 1}\nd.get(default = 9)"), "required"));
    CHECK(has(err("var d = {\"a\": 1}\nd.pop(default = 9)"), "required"));
    // normal calls (positional, all-keyword, or key + keyword default) still work:
    CHECK(ok("var d = {}\nd.setdefault(\"k\", 7)\nd[\"k\"]") == "7");
    CHECK(ok("var d = {\"a\": 1}\nd.get(key = \"a\")") == "1");
    CHECK(ok("var d = {\"a\": 1}\nd.get(key = \"z\", default = 9)") == "9");
    CHECK(ok("var d = {}\nd.setdefault(key = \"k\", default = 7)\nd[\"k\"]") == "7");
    // an OPTIONAL leading param may still be skipped by naming a later one (regression guard):
    CHECK(ok("\"a b c\".split(maxsplit = 1)") == "['a', 'b c']");

    // === A03-3: a parameter default may reference an EARLIER parameter (the binder defines params
    // left-to-right). The resolver used to reject it as "name not defined"; now it resolves, and the
    // default binds the actual parameter (not an enclosing same-named value). ===
    CHECK(ok("var f = Function(a, b = a): return [a, b]\nvar a = 999\nf(7)") == "[7, 7]");
    CHECK(ok("var pow2 = Function(base, exp = base): return base ** exp\npow2(3)") == "27");
    // referencing a LATER parameter is still correctly rejected (not yet bound at default-eval time):
    CHECK(has(err("var g = Function(a, b = c, c = 1): return a\n"), "not defined"));

    // === A01-1: `\xHH` in a String literal is code point U+00HH (UTF-8-encoded), not a raw byte, so a
    // high byte no longer merges with the following byte under the code-point layer. ===
    CHECK(ok("len(\"\\xC3\\x28\")") == "2");    // U+00C3 then '(' — two code points (Python 3 parity)
    CHECK(ok("ord(\"\\xC3\")") == "195");        // the single code point's value
    CHECK(ok("len(\"\\xff\")") == "1");
    CHECK(ok("ord(\"\\x41\")") == "65");         // ASCII is unchanged (1 byte)
    // encode round-trips: U+00FF is 2 UTF-8 bytes / 1 latin-1 byte, and decodes back equal.
    CHECK(ok("len(\"\\xff\".encode(\"utf-8\"))") == "2");     // UTF-8: U+00FF -> 0xC3 0xBF
    CHECK(ok("len(\"\\xff\".encode(\"latin-1\"))") == "1");   // latin-1: one byte 0xFF
    CHECK(ok("\"\\xff\".encode(\"latin-1\").decode(\"latin-1\") == \"\\xff\"") == "True");
    CHECK(ok("\"\\xff\".encode(\"utf-8\").decode(\"utf-8\") == \"\\xff\"") == "True");

    // (A06-7 — "same NaN object should dedupe in a Set/Dict" — was reverted: NaN write-only keys are a
    // documented invariant, r7_types.ki. Left to a maintainer design decision.)

    // === A20-5: tabular sortvalues must not crash on missing values; they sort to the END regardless
    // of direction (pandas na_position='last'), and a Float NaN counts as missing too (not just None). ===
    CHECK(ok("var t = import(\"tabular\")\nvar s = t.Series([3, None, 1])\n"
             "s.sortvalues().tolist()") == "[1, 3, None]");
    // DESCENDING: missing still last (the old [flag,value] key wrongly put None first under reverse)
    CHECK(ok("var t = import(\"tabular\")\nvar s = t.Series([3, None, 1])\n"
             "s.sortvalues(False).tolist()") == "[3, 1, None]");
    // Float NaN is treated as missing (via _isnan), sorted last, not left in place
    CHECK(ok("var t = import(\"tabular\")\nvar m = import(\"math\")\n"
             "var s = t.Series([3.0, m.nan, 1.0])\n"
             "s.sortvalues().tolist()") == "[1.0, 3.0, nan]");
    // multiple missing, both directions
    CHECK(ok("var t = import(\"tabular\")\nvar s = t.Series([3, None, 1, None, 2])\n"
             "s.sortvalues(False).tolist()") == "[3, 2, 1, None, None]");
    CHECK(ok("var t = import(\"tabular\")\n"
             "var df = t.DataFrame({\"a\": [3, None, 1], \"b\": [1, 2, 3]})\n"
             "df.sortvalues(\"a\").column(\"a\").tolist()") == "[1, 3, None]");
    CHECK(ok("var t = import(\"tabular\")\n"
             "var df = t.DataFrame({\"a\": [3, None, 1], \"b\": [1, 2, 3]})\n"
             "df.sortvalues(\"a\", False).column(\"a\").tolist()") == "[3, 1, None]");

    // === A20-4: functools.reduce must treat an explicit initial=None as a real seed (identity
    // sentinel), not "no initial supplied". reduce(f, [], None) -> None; reduce(f, [1,2,3]) still folds. ===
    CHECK(ok("var f = import(\"functools\")\nf.reduce(Function(a, b): return a + b, [], None)") == "None");
    CHECK(ok("var f = import(\"functools\")\nf.reduce(Function(a, b): return a + b, [1, 2, 3])") == "6");
    CHECK(ok("var f = import(\"functools\")\nf.reduce(Function(a, b): return a + b, [1, 2, 3], 10)") == "16");
    CHECK(has(err("var f = import(\"functools\")\nf.reduce(Function(a, b): return a + b, [])"),
              "empty sequence"));  // still throws when genuinely unset + empty

    // === A05-2: a keyword call that skips a REQUIRED leading arg errors with "missing required
    // argument", not a misleading downstream type error from a None-filled slot. ===
    CHECK(has(err("var xs = [1]\nxs.insert(item = 99)"), "missing required argument"));  // index skipped
    CHECK(has(err("var xs = [1]\nxs.insert(item = 99)"), "index"));                      // names the slot
    CHECK(ok("var xs = [1, 2]\nxs.insert(index = 1, item = 99)\nxs") == "[1, 99, 2]");   // both keywords work
    CHECK(ok("var xs = [1, 2]\nxs.insert(0, 9)\nxs") == "[9, 1, 2]");                    // positional unchanged
    // the previously-silent Dict cases still throw "missing required argument" (regression from A05-2)
    CHECK(has(err("var d = {1: 2}\nd.setdefault(default = 7)"), "missing required argument"));
    CHECK(has(err("var d = {1: 2}\nd.get(default = 9)"), "missing required argument"));
    CHECK(has(err("var d = {1: 2}\nd.pop(default = 9)"), "missing required argument"));

    // === A02-2: an error inside an f-string reports the f-string's REAL source line, not line 1.
    // The embedded {expr} is sub-lexed seeded at the token's line/col, so its node spans are absolute. ===
    CHECK(errLine("var x = 0\n\n\nf\"{1 / x}\"\n") == 4);        // runtime div-by-zero on line 4
    CHECK(errLine("\n\nf\"{undefined_name_here}\"\n") == 3);     // resolver undefined-name on line 3
    // a control (same error NOT in an f-string) still reports its own line, unchanged
    CHECK(errLine("var x = 0\n\n\nvar y = 1 / x\n") == 4);

    // === A05-4: a non-Bool _bool_ used as an if/while CONDITION reports the condition's real line,
    // not 0:0 (the JumpIf* condition path now wraps truthy() in located()). ===
    CHECK(errLine("class Bad:\n  var _bool_ = Function(self): return 5\n\nwhile Bad():\n  break\n") == 4);
    CHECK(errLine("class Bad:\n  var _bool_ = Function(self): return 5\n\nif Bad():\n  pass\n") == 4);

    // === A07-3: privacy is per class *chain*, not per defining class — a subclass method may read a
    // base(-typed) instance's private, symmetric with a base method reading a derived instance's.
    // Previously only base->derived worked (the check was isInstanceOf(obj, currentClass), one-directional). ===
    // subclass method reading a base instance's inherited private via self (already worked):
    CHECK(ok("class B:\n  var _init_ = Function(self): self._x = 10\n"
             "class D(B):\n  var read = Function(self): return self._x\nD().read()") == "10");
    // subclass method reading a SEPARATE base instance's private (the fixed case):
    CHECK(ok("class B:\n  var _init_ = Function(self): self._x = 10\n"
             "class D(B):\n  var peek = Function(self, o): return o._x\nD().peek(B())") == "10");
    // base method reading a derived instance's private (was and stays allowed — symmetry):
    CHECK(ok("class B:\n  var _init_ = Function(self): self._x = 10\n"
             "  var peek = Function(self, o): return o._x\n"
             "class D(B):\n  var go = Function(self): return 0\nB().peek(D())") == "10");
    // an UNRELATED class still cannot reach into the private member:
    CHECK(has(err("class B:\n  var _init_ = Function(self): self._x = 10\n"
                  "class O:\n  var peek = Function(self, o): return o._x\nO().peek(B())"),
              "cannot access private member"));
    // and plain outside code still cannot:
    CHECK(has(err("class B:\n  var _init_ = Function(self): self._x = 10\nB()._x"),
              "cannot access private member"));

    return RUN_TESTS();
}
