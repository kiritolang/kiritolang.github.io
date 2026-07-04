// Regression tests for the v1.12 hardening pass (audit_v1_12/FINDINGS.md). Each block pins one
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
    CHECK(okGc1("len(map(Function(c): return c + \"!\", \"abcdefghij\"))") == "10");  // A09-4 map
    CHECK(okGc1("len(filter(Function(c): return True, \"abcdefghij\"))") == "10");    // A09-4 filter
    CHECK(okGc1("sorted(\"dbca\")") == "['a', 'b', 'c', 'd']");                    // A09-4 sorted (key/_lt_)
    CHECK(okGc1("all(map(Function(x): return True, \"abcde\"))") == "True");       // A09-4 all
    CHECK(okGc1("any(map(Function(x): return False, \"abcde\"))") == "False");     // A09-4 any
    CHECK(okGc1("len(zip(\"abc\", \"defgh\"))") == "3");                           // zip columns
    CHECK(okGc1("len(enumerate(\"abcde\"))") == "5");                              // enumerate
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

    // === TENSOR-2: einsum's contraction count is the product of ALL label sizes; unchecked it
    // overflows size_t (silent wrong) or hangs (DoS). An oversized contraction must throw. Here
    // i*j*k = 1000^3 = 1e9 > the 64M element cap (the operands themselves are only 1e6 each). ===
    CHECK(has(err("var t = import(\"tensor\")\nvar a = t.ones([1000, 1000])\n"
                  "discard t.einsum(\"ij,jk->ik\", a, a)"), "too large"));
    CHECK(ok("var t = import(\"tensor\")\nt.einsum(\"ij,jk->ik\", t.eye(3), t.eye(3)).tolist()")
          == "[[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]");  // normal einsum still works

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

    return RUN_TESTS();
}
