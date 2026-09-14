// Function inlining (compile-time, hygienic) — v1: capture-free single-return flat lambdas.
// The load-bearing guarantee is "inlining changes SPEED, not RESULTS": every program must produce a
// byte-identical value (and identical thrown-error text) with the inline transform ON vs. OFF. This TU
// is the differential harness for that, plus targeted hygiene / side-effect-once / firing checks.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run `src` in a fresh stdlib VM; return stringified result, or "ERR:<msg>" if it threw.
static std::string run1(bool inlining, const std::string& src) {
    KiritoVM vm;
    vm.installStandardLibrary();
    vm.setInliningEnabled(inlining);
    try { return vm.stringify(vm.runSource(src)); }
    catch (const KiritoError& e) { return std::string("ERR:") + e.what(); }
    catch (const std::exception& e) { return std::string("STD:") + e.what(); }
}
// The core invariant: identical result with inlining on and off.
static bool same(const std::string& src) { return run1(true, src) == run1(false, src); }
// Result with inlining ON (to also pin the actual value, not just on==off).
static std::string on(const std::string& src) { return run1(true, src); }

int main() {
    // ---- differential battery: on == off for every shape (inlinable AND non-inlinable) ----
    // v1 inlining fires only for a directly-called capture-free single-return flat lambda inside a
    // function; every OTHER shape must still be identical because both take the normal call path.
    const char* battery[] = {
        // inlinable (fires): direct-literal call inside a function
        "var f = Function():\n return (Function(x): return x * x)(6)\nf()",
        // multiple param references — arg evaluated once, read twice
        "var f = Function():\n return (Function(x): return x + x + x)(7)\nf()",
        // nested inline: a body that itself directly-calls another inlinable literal
        "var f = Function():\n return (Function(x): return (Function(z): return z + 1)(x) * 2)(10)\nf()",
        // body references a builtin (capture-free: free var is a global)
        "var f = Function():\n return (Function(s): return len(s) + 1)(\"abcd\")\nf()",
        // conditional expression body
        "var f = Function():\n return (Function(n): return \"even\" if n % 2 == 0 else \"odd\")(9)\nf()",
        // NOT inlinable (annotated) — still equal (both normal-call)
        "var f = Function():\n return (Function(x : Integer) -> Integer: return x + 1)(4)\nf()",
        // NOT inlinable (multi-statement body) — still equal
        "var f = Function():\n var g = Function(x):\n  var y = x + 1\n  return y * 2\n return g(5)\nf()",
        // NOT inlinable (capturing lambda) — still equal (normal closure)
        "var f = Function():\n var base = 100\n return (Function(x): return x + base)(5)\nf()",
        // NOT inlinable (module scope, slots disabled) — still equal
        "(Function(x): return x * 3)(11)",
        // a thrown error is identical text on/off
        "var f = Function():\n return (Function(x): return x / 0)(1)\nf()",
        "var f = Function():\n return (Function(x): return [1, 2][x])(9)\nf()",  // index OOB throws
        // module-scope inline (B fires at top level too via hidden bindings)
        "(Function(x): return x * 4 + 1)(9)",
        // Part C: fused `for x in map/filter(lambda, src)` — both eager and inside a function
        "var out = []\nfor x in map(Function(v): return v * v, range(6)):\n out.append(x)\nout",
        "var out = []\nfor x in filter(Function(v): return v % 3 == 0, range(12)):\n out.append(x)\nout",
        "var f = Function(xs):\n var s = 0\n for x in map(Function(v): return v + 1, xs):\n  s = s + x\n return s\nf([1, 2, 3])",
        // Part C with break/continue in the fused body
        "var out = []\nfor x in map(Function(v): return v, range(10)):\n if x == 2:\n  continue\n if x == 5:\n  break\n out.append(x)\nout",
        // Part C not lowered (callback captures) — still equal via the normal combinator
        "var k = 10\nvar out = []\nfor x in map(Function(v): return v + k, [1, 2]):\n out.append(x)\nout",
        // Part C not lowered (callback is a bound name, not a literal) — still equal
        "var sq = Function(v): return v * v\nvar out = []\nfor x in map(sq, [3, 4]):\n out.append(x)\nout",
    };
    for (const char* s : battery) CHECK(same(s));

    // ---- pin actual values (inlining ON) ----
    CHECK(on("var f = Function():\n return (Function(x): return x * x)(6)\nf()") == "36");
    CHECK(on("var f = Function():\n return (Function(x): return x + x + x)(7)\nf()") == "21");
    CHECK(on("var f = Function():\n return (Function(x): return (Function(z): return z + 1)(x) * 2)(10)\nf()") == "22");
    CHECK(on("var f = Function():\n return (Function(s): return len(s) + 1)(\"abcd\")\nf()") == "5");
    // Part C: fused map/filter produce the exact expected sequence
    CHECK(on("var out = []\nfor x in map(Function(v): return v * v, range(6)):\n out.append(x)\nout")
          == "[0, 1, 4, 9, 16, 25]");
    CHECK(on("var out = []\nfor x in filter(Function(v): return v % 3 == 0, range(12)):\n out.append(x)\nout")
          == "[0, 3, 6, 9]");
    CHECK(on("var out = []\nfor x in map(Function(v): return v, range(10)):\n if x == 2:\n  continue\n"
             " if x == 5:\n  break\n out.append(x)\nout") == "[0, 1, 3, 4]");

    // ---- hygiene: an inlined body's param must NOT see a caller local of the same name, and the
    //      caller's binding must be untouched ("fake shadowing" resolves to distinct slots) ----
    CHECK(on("var f = Function():\n var y = 100\n var r = (Function(y): return y + 1)(7)\n return [r, y]\nf()")
          == "[8, 100]");
    CHECK(same("var f = Function():\n var y = 100\n var r = (Function(y): return y + 1)(7)\n return [r, y]\nf()"));
    // two sibling inlines in one function with same-named internals do not cross-reference
    CHECK(on("var f = Function():\n var a = (Function(x): return x + 1)(10)\n"
             " var b = (Function(x): return x + 2)(20)\n return [a, b]\nf()") == "[11, 22]");

    // ---- side-effect-once: the argument expression is evaluated exactly once even when the param is
    //      read many times (and once even when the param is unused) ----
    CHECK(on("var log = []\n"
             "var bump = Function():\n log.append(1)\n return 3\n"
             "var f = Function():\n return (Function(x): return x + x + x)(bump())\n"
             "var r = f()\n[r, len(log)]") == "[9, 1]");
    CHECK(on("var log = []\n"
             "var bump = Function():\n log.append(1)\n return 3\n"
             "var f = Function():\n return (Function(x): return 42)(bump())\n"   // param unused
             "var r = f()\n[r, len(log)]") == "[42, 1]");

    // (Firing — that a candidate call actually becomes inline codegen, not a normal call — is confirmed
    // out-of-band: the CLI traceback of an inlined body shows no separate lambda frame, whereas
    // `--no-inline` restores it; and the bench workload shows the speedup. Here we assert correctness
    // and on==off equivalence, which is the load-bearing guarantee.)

    return RUN_TESTS();
}
