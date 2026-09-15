// Function inlining (compile-time, hygienic) — v1: capture-free single-return flat lambdas.
// The load-bearing guarantee is "inlining changes SPEED, not RESULTS": every program must produce a
// byte-identical value (and identical thrown-error text) with the inline transform ON vs. OFF. This TU
// is the differential harness for that, plus targeted hygiene / side-effect-once / firing checks.
#include <initializer_list>
#include <iterator>
#include <random>
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
        // const-bound-local: a `var f = <lambda>` called by name inlines (fires); result identical
        "var sq = Function(x): return x * x\nvar t = 0\nfor i in range(5):\n t = t + sq(i)\nt",
        "var f = Function():\n var sq = Function(x): return x * x\n return sq(6) + sq(7)\nf()",
        // reassigned binding -> NOT tracked (mutable), still equal
        "var g = Function(x): return x + 1\ng = Function(x): return x + 100\ng(5)",
        // recursive const -> NOT inlined (references itself: a non-global free var), still equal
        "var fac = Function(n): return 1 if n <= 1 else n * fac(n - 1)\nfac(6)",
        // REGRESSION: a const helper using a builtin name that is SHADOWED by a lexical var must NOT be
        // mis-inlined (the shadow resolves to a LoadVar, not a global) — this once tripped a bad LoadVar.
        "var f = Function():\n var len = Function(x): return 999\n var use = Function(s): return len(s)\n return use(\"ab\")\nf()",
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
    // const-bound-local inlined helper produces the right value
    CHECK(on("var sq = Function(x): return x * x\nvar t = 0\nfor i in range(5):\n t = t + sq(i)\nt") == "30");
    CHECK(on("var fac = Function(n): return 1 if n <= 1 else n * fac(n - 1)\nfac(6)") == "720");

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

    // ---- property/fuzz: randomly assemble small programs mixing inlinable literals, const-bound-local
    //      helpers, map/filter fusion, captures, shadowing, and non-inlinable shapes; assert inline
    //      ON == OFF for every one. Seeded for reproducibility. This is the deep guard on "speed, not
    //      results" — it exercises far more shapes (and their interactions) than the fixed battery. ----
    {
        std::mt19937 rng(0xC0FFEE);
        auto pick = [&](std::initializer_list<const char*> xs) {
            auto it = xs.begin(); std::advance(it, rng() % xs.size()); return std::string(*it);
        };
        int mismatches = 0;
        for (int iter = 0; iter < 400; ++iter) {
            // a random pure expression over the loop variable `e` and a captured `base`
            std::string expr = pick({"e", "e * e", "e + base", "e - 1", "e * 2 + base", "len(String(e))",
                                     "e % 3", "(e + base) * 2", "e * e - base"});
            std::string pred = pick({"e % 2 == 0", "e > base", "e < 5", "e != 3", "e >= 0"});
            std::string src = "var base = " + std::to_string(static_cast<int>(rng() % 7)) + "\n";
            int shape = static_cast<int>(rng() % 7);
            if (shape == 0)        // direct-literal call inside a function
                src += "var f = Function():\n return (Function(e): return " + expr + ")(" +
                       std::to_string(static_cast<int>(rng() % 9)) + ")\nf()";
            else if (shape == 1)   // const-bound-local helper called in a loop
                src += "var h = Function(e): return " + expr + "\nvar t = 0\nfor i in range(6):\n"
                       " t = t + h(i)\nt";
            else if (shape == 2)   // fused map over a range
                src += "var out = []\nfor x in map(Function(e): return " + expr + ", range(6)):\n"
                       " out.append(x)\nout";
            else if (shape == 3)   // fused filter over a range
                src += "var out = []\nfor x in filter(Function(e): return " + pred + ", range(8)):\n"
                       " out.append(x)\nout";
            else if (shape == 4)   // map whose callback captures `base` (not fused) — still must agree
                src += "var out = []\nfor x in map(Function(e): return e + base, range(5)):\n"
                       " out.append(x)\nout";
            else if (shape == 5)   // nested: fused map with a directly-inlined sub-call in the body
                src += "var out = []\nfor x in map(Function(e): return (Function(z): return z + 1)(e), range(5)):\n"
                       " out.append(x)\nout";
            else                   // TRICKY: the loop body MUTATES the captured var — auto-lift must
                                   // read the capture per element (closure-by-reference), not once
                src += "var out = []\nfor x in map(Function(e): return " + expr + ", range(5)):\n"
                       " base = base + 1\n out.append(x)\nout";
            if (run1(true, src) != run1(false, src)) ++mismatches;
        }
        CHECK(mismatches == 0);
    }

    // (Firing — that a candidate call actually becomes inline codegen, not a normal call — is confirmed
    // out-of-band: the CLI traceback of an inlined body shows no separate lambda frame, whereas
    // `--no-inline` restores it; and the bench workload shows the speedup. Here we assert correctness
    // and on==off equivalence, which is the load-bearing guarantee.)

    // ---- adversarial edge battery (each empirically hunted for on!=off divergence) ----
    const char* edges[] = {
        // top-level (module-scope) capture: the body reads a module var; rebind must resolve it in place
        "var base = 10\n(Function(x): return x + base)(5)",
        // a nested function that CALLS a const-fn from an OUTER scope must NOT inline it (const tracking is
        // per-proto) and must still compute correctly via a normal call
        "var f = Function():\n var base = 100\n var g = Function(x): return x + base\n"
        " var h = Function():\n  return g(5)\n return h()\nf()",
        // an inner `var` shadows an outer const-fn name -> inner is the callee, outer is untouched
        "var sq = Function(x): return x * x\n"
        "var f = Function():\n var sq = Function(x): return x + 1000\n return sq(5)\nf()",
        // a const-fn used BOTH inlined (by name) AND passed first-class (the shared AST must not be mutated)
        "var sq = Function(x): return x * x\n[sq(6), List(map(sq, range(4)))]",
        // deep const-fn chain (each calls the previous) — inlines nest, value stays correct
        "var a = Function(x): return x + 1\nvar b = Function(x): return a(x) + 1\n"
        "var c = Function(x): return b(x) + 1\nvar d = Function(x): return c(x) + 1\nd(10)",
        // fused for + return out of the enclosing function from inside the fused body
        "var f = Function(xs):\n for x in map(Function(v): return v * 10, xs):\n  if x == 20:\n   return x\n"
        " return -1\nf([1, 2, 3])",
        // fused filter with a side-effecting predicate: evaluated exactly once per element, in order
        "var log = []\nvar out = []\nfor x in filter(Function(v):\n log.append(v)\n return v % 2 == 0\n, range(5)):\n"
        " out.append(x)\n[out, log]",
        // fused loop body MUTATES the captured var between elements — capture must be read in place
        "var base = 0\nvar out = []\nfor x in map(Function(e): return e + base, range(5)):\n"
        " base = base + 1\n out.append(x)\nout",
        // eager consumers over map/filter (NOT fused — the lazy builtin path; must be identical on/off)
        "List(map(Function(x): return x * x, range(6)))",
        "List(filter(Function(x): return x % 2 == 0, range(10)))",
        "List(Set(map(Function(x): return x % 3, range(9))))",
        "sum(map(Function(x): return x * x, range(5)))",
        "len(List(filter(Function(x): return x > 3, range(20))))",
        // eager consumer over a NON-literal callback (a const-fn name) — normal lazy path
        "var f = Function(x): return x + 1\nList(map(f, range(4)))",
        // a captured CONTAINER may be mutated from a nested function (only rebinding the NAME is banned)
        "var f = Function():\n var acc = []\n var g = Function(x): discard acc.append(x)\n g(1)\n g(2)\n"
        " return acc\nf()",
        // nested inline literal inside a fused map body
        "var out = []\nfor x in map(Function(e): return (Function(z): return z + 1)(e), range(5)):\n"
        " out.append(x)\nout",
        // fused map over a string source (iterates characters)
        "var out = []\nfor c in map(Function(ch): return ch + \"!\", \"abc\"):\n out.append(c)\nout",
    };
    for (const char* s : edges) CHECK(same(s));

    // pin a few adversarial values (inlining ON)
    CHECK(on("var base = 10\n(Function(x): return x + base)(5)") == "15");
    CHECK(on("var f = Function():\n var base = 100\n var g = Function(x): return x + base\n"
             " var h = Function():\n  return g(5)\n return h()\nf()") == "105");
    CHECK(on("var sq = Function(x): return x * x\n"
             "var f = Function():\n var sq = Function(x): return x + 1000\n return sq(5)\nf()") == "1005");
    CHECK(on("var base = 0\nvar out = []\nfor x in map(Function(e): return e + base, range(5)):\n"
             " base = base + 1\n out.append(x)\nout") == "[0, 2, 4, 6, 8]");
    CHECK(on("var f = Function():\n var acc = []\n var g = Function(x): discard acc.append(x)\n g(1)\n"
             " g(2)\n return acc\nf()") == "[1, 2]");

    // ---- write-through-closure ban: fires identically WITH and WITHOUT inlining (validation-always-on),
    //      and rejects the SAME program in both modes with the SAME structured message ----
    {
        const char* rebindCapture =
            "var f = Function():\n var c = 0\n var g = Function():\n  c = c + 1\n  return c\n return g()\nf()";
        std::string onR = run1(true, rebindCapture), offR = run1(false, rebindCapture);
        CHECK(onR == offR);                                          // rejected identically in both modes
        CHECK(onR.rfind("ERR:", 0) == 0);                            // it is an error (compile-time)
        CHECK(onR.find("captured from an enclosing function") != std::string::npos);  // the exact reason
        // a tuple-unpack target that rebinds a captured var is banned too
        const char* tupleCapture =
            "var f = Function():\n var a = 1\n var g = Function():\n  a, b = 2, 3\n  return a\n return g()\nf()";
        CHECK(run1(true, tupleCapture) == run1(false, tupleCapture));
        CHECK(run1(true, tupleCapture).find("captured from an enclosing function") != std::string::npos);
        // LEGITIMATE writes must NOT be rejected (module global, own local, captured-container element):
        CHECK(run1(true, "var n = 0\nvar f = Function(): return n\nn = 5\nf()") == "5");   // module global write
        CHECK(run1(true, "var f = Function():\n var box = [0]\n var g = Function():\n  box[0] = box[0] + 1\n"
                         "  return box[0]\n return g() + g()\nf()") == "3");                 // captured container
    }

    // (GC soak — inlined bodies / fused loops / hidden $inl/$cmb slots keeping every intermediate handle
    // rooted — is exercised at the user-code layer by the `--gc-threshold 1` .ki soak tests
    // script_spec_cheapcall_soak / script_spec_inline_adversarial_soak (and their _noinl differentials),
    // which run under ASan. The differential harness here owns the on==off invariant over the FUZZED
    // space, which a fixed .ki golden cannot express.)

    // ---- flag-latch guard (embedding API): a fresh VM with the flag set BEFORE running is the supported
    //      usage and both modes yield identical values; this locks that setInliningEnabled is honored and
    //      that inlining is result-preserving even under the debug switch. ----
    {
        KiritoVM v1; v1.installStandardLibrary(); v1.setInliningEnabled(true);
        KiritoVM v2; v2.installStandardLibrary(); v2.setInliningEnabled(false);
        const char* prog = "var sq = Function(x): return x * x\nvar t = 0\nfor i in range(10):\n t = t + sq(i)\nt";
        CHECK(v1.stringify(v1.runSource(prog)) == v2.stringify(v2.runSource(prog)));
        CHECK(v1.inliningEnabled() == true && v2.inliningEnabled() == false);
    }

    // ================================================================================================
    // InlineFunction (the opt-in guaranteed-inline keyword): single- AND multi-statement bodies,
    // defaults, keyword args, and enforced annotations. The load-bearing invariant is the SAME as
    // above — a program that INLINES must produce a byte-identical value (and identical thrown-error
    // text) with inlining ON vs. --no-inline (which demotes InlineFunction to a plain Function). Cases
    // that DON'T inline (arity/keyword/starred/value-use compile errors) are the documented asymmetry
    // and are asserted separately, NOT via on==off.
    // ================================================================================================
    {
        // valid inline shapes: every one must be on==off (result or identical thrown message)
        const char* ifBattery[] = {
            // immediately-applied literal, single expression
            "(InlineFunction(a, b): return a * a + b * b)(3, 4)",
            // const-bound, multi-statement body with an early return
            "var f = InlineFunction(x):\n if x < 0:\n  return 0 - x\n return x\n[f(-7), f(7)]",
            // body locals that shadow a caller local and a module global
            "var x = 99\nvar g = 5\nvar h = InlineFunction(x):\n var g = x + 1\n return g\n[h(41), x, g]",
            // default value (literal) + a default referencing an earlier parameter
            "var p = InlineFunction(base, exp = 2): return base ** exp\n"
            "var s = InlineFunction(lo, hi = lo + 5): return hi - lo\n[p(5), p(2, 10), s(10), s(10, 30)]",
            // keyword arguments, out of order, plus positional+keyword mix
            "var sub = InlineFunction(a, b): return a - b\n[sub(b = 3, a = 10), sub(10, b = 4)]",
            // single, union, None-in-union annotations that ACCEPT
            "var t = InlineFunction(x: Integer) -> Integer: return x + 1\n"
            "var u = InlineFunction(x: [Integer, Float]): return x * 2\n[t(41), u(3), u(2.5)]",
            // annotation VIOLATIONS — thrown at run time with the SAME message inlined vs demoted
            "(InlineFunction(x: Integer): return x)(3.5)",
            "(InlineFunction(x: [Integer, Float]): return x)(\"no\")",
            "(InlineFunction(x) -> Integer: return x)(\"hi\")",
            "var m = InlineFunction(x) -> Integer:\n if x > 0:\n  return x\n(InlineFunction(): return m(-1))()",
            // nested InlineFunction calls
            "var inc = InlineFunction(x): return x + 1\n"
            "var inc2 = InlineFunction(x):\n var a = inc(x)\n return inc(a)\ninc2(40)",
            // once-only side-effecting argument
            "var c = 0\nvar bump = Function() -> Integer:\n c = c + 1\n return c\n"
            "var tw = InlineFunction(v): return v + v\n[tw(bump()), c]",
            // param-container mutation reaches the caller's object
            "var lst = [1, 2, 3]\nvar setf = InlineFunction(b, v):\n b[0] = v\n return b\n[setf(lst, 9), lst]",
            // const InlineFunction used cross-scope (materialized) + as a first-class value
            "var sq = InlineFunction(x): return x * x\n"
            "var apply = Function(fn, n): return fn(n)\n[sq(6), apply(sq, 7)]",
        };
        for (const char* s : ifBattery) CHECK(same(s));

        // pin actual values (inlining ON) — independent of the on==off check
        CHECK(on("(InlineFunction(a, b): return a * a + b * b)(3, 4)") == "25");
        CHECK(on("var p = InlineFunction(base, exp = 2): return base ** exp\n[p(5), p(2, 10)]") == "[25, 1024]");
        CHECK(on("(InlineFunction(x: Integer): return x)(3.5)") ==
              "ERR:argument 'x' must be Integer, got Float");
        CHECK(on("var m = InlineFunction(x) -> Integer:\n if x > 0:\n  return x\n"
                 "(InlineFunction(): return m(-1))()") == "ERR:function must return Integer, got None");

        // independent oracle: an InlineFunction and the SAME-bodied Function agree on the result.
        {
            KiritoVM vi; vi.installStandardLibrary(); vi.setInliningEnabled(true);
            KiritoVM vf; vf.installStandardLibrary();
            for (int k = -5; k <= 20; ++k) {
                std::string body = " if x % 2 == 0:\n  return x * x + 3\n return x - 100\n";
                std::string inl = "var f = InlineFunction(x):\n" + body + "f(" + std::to_string(k) + ")";
                std::string fun = "var f = Function(x):\n" + body + "f(" + std::to_string(k) + ")";
                CHECK(vi.stringify(vi.runSource(inl)) == vf.stringify(vf.runSource(fun)));
            }
        }
    }

    // ---- the documented --no-inline ASYMMETRY: a program the inliner rejects at compile time is a LOUD
    //      error with inlining ON, yet compiles/runs under --no-inline (InlineFunction demotes to
    //      Function). Assert exactly that direction — ON errors, OFF does not — for each rejected shape.
    {
        const char* rejected[] = {
            "var apply = Function(fn, x): return fn(x)\napply(InlineFunction(x): return x * 2, 5)",  // literal as value
            "var f = InlineFunction(x): return x\nvar g = f\ng(1)",                                   // index 1: VALID (alias)
            "var fac = InlineFunction(n): return 1 if n < 2 else n * fac(n - 1)\nfac(5)",             // recursion
            "(InlineFunction(x): return x)(1, 2)",                                                    // arity
            "(InlineFunction(x): return x)(y = 1)",                                                   // unknown keyword
            "var f = InlineFunction(n):\n var s = 0\n for i in range(n):\n  s = s + i\n return s\nf(4)",// loop body
            "class C:\n var m = InlineFunction(self): return 1\nC().m()",                             // method
        };
        // Skip index 1 (aliasing a const InlineFunction is VALID now — it materializes); check the rest.
        for (std::size_t i = 0; i < std::size(rejected); ++i) {
            if (i == 1) { CHECK(run1(true, rejected[i]) == run1(false, rejected[i])); continue; }  // valid: on==off
            std::string onR = run1(true, rejected[i]), offR = run1(false, rejected[i]);
            CHECK(onR.rfind("ERR:", 0) == 0);                 // inlining ON: a loud compile error
            CHECK(onR.find("InlineFunction") != std::string::npos);  // ...naming the construct
            CHECK(onR != offR);                               // --no-inline does NOT hit that error
        }
        // the literal value-use ban names the fix
        CHECK(run1(true, "var apply = Function(fn, x): return fn(x)\napply(InlineFunction(x): return x, 5)")
                  .find("cannot be used as a value") != std::string::npos);
    }

    // ---- InlineFunction property/fuzz: random multi-statement bodies (var/if/return over the params and
    //      builtins only, so they always inline), random defaults + positional/keyword call shapes;
    //      assert inline ON == --no-inline for every one. Seeded. ----
    {
        std::mt19937 rng(0x1FCE);
        auto pick = [&](std::initializer_list<const char*> xs) {
            auto it = xs.begin(); std::advance(it, rng() % xs.size()); return std::string(*it);
        };
        int mismatches = 0;
        for (int iter = 0; iter < 500; ++iter) {
            std::string e1 = pick({"a", "a * a", "a + b", "a - b", "a % 3", "len(String(a))", "a * b - 1"});
            std::string e2 = pick({"b", "b + 1", "a * b", "b * b", "a + b + 1"});
            std::string pred = pick({"a > b", "a % 2 == 0", "a < 0", "b != 0", "a >= b"});
            bool hasDefault = (rng() % 2) == 0;
            std::string decl = hasDefault ? "var f = InlineFunction(a, b = a + 1):\n"
                                          : "var f = InlineFunction(a, b):\n";
            // body: a temp, a branch with an early return, a fall-through return
            std::string body = " var t = " + e1 + "\n if " + pred + ":\n  return t\n return " + e2 + "\n";
            int av = static_cast<int>(rng() % 11) - 5;
            int bv = static_cast<int>(rng() % 11) - 5;
            std::string call;
            int shape = static_cast<int>(rng() % 4);
            if (shape == 0)      call = "f(" + std::to_string(av) + ", " + std::to_string(bv) + ")";
            else if (shape == 1) call = "f(b = " + std::to_string(bv) + ", a = " + std::to_string(av) + ")";
            else if (shape == 2 && hasDefault) call = "f(" + std::to_string(av) + ")";  // use the default
            else                 call = "f(" + std::to_string(av) + ", " + std::to_string(bv) + ")";
            std::string src = decl + body + call;
            if (run1(true, src) != run1(false, src)) ++mismatches;
        }
        CHECK(mismatches == 0);
    }

    return RUN_TESTS();
}
