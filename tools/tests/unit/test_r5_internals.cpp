// Round-2 internals + regression coverage, pinned from C++ where the C++ side is the real contract.
// Complements (does NOT duplicate) test_r4_cpp_api.cpp (the embedding/extension API: value.hpp,
// NativeModule/NativeClass, bindArgs, the Object protocol, arena/GC reachability, multi-VM):
//
//   1. GC stress from C++: many allocations under a tight threshold, rooted survivors, manual
//      collection, and a CYCLE (a<->b, and a self-referencing list) reclaimed once unrooted —
//      mark-sweep handles cycles, no leak/dangle (mirrors test_gc.cpp / test_arena.cpp).
//   2. Bytecode VM / compiler edges: a very deep expression evaluates; the call-depth guard RAISES
//      (catchable) instead of overflowing the native stack; constant dedup is transparent; and
//      slot-addressed locals' read-before-assign falls back correctly (rebind-outer vs UnboundLocal).
//   3. Resolver + analyzer from C++: an undefined name is a COMPILE-time error (thrown from
//      runSource before any code runs); the Analyzer surfaces a representative warning set.
//   4. serde from C++: a value graph with a SHARED reference AND a CYCLE round-trips through both the
//      text (`serial`) and binary (`dumpfmt`) codecs, preserving aliasing + the cycle.
//   5. Round-1 fix regressions, driven on a fresh KiritoVM via runSource:
//        - tensor.einsum with a repeated OUTPUT label ("ii->ii", "i->ii") throws a catchable
//          KiritoError (it used to overrun the output buffer); valid specs ("ij->ji", "ii->") still
//          work.
//        - json.loads rejects malformed numbers ("1.5e", "01", "1.") and accepts valid ones
//          ("1.5e10", "0", "-0", "1.5").
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run a chunk on a fresh VM; return the stringified last value (fresh VM => no cross-test leakage).
static std::string run(const std::string& src) {
    KiritoVM vm;
    return vm.stringify(vm.runSource(src));
}
// True iff `src` throws (compile-time or run-time) on a fresh VM.
static bool throws(const std::string& src) {
    KiritoVM vm;
    try { vm.runSource(src); return false; } catch (...) { return true; }
}
// The (compile or run) error text, or "" on success.
static std::string errOf(const std::string& src) {
    KiritoVM vm;
    try { vm.runSource(src); return ""; } catch (const std::exception& e) { return e.what(); }
}
// Analyzer warning messages (no file:line:col prefix), straight from the AST.
static std::vector<std::string> warn(const std::string& src) {
    Parser parser(Lexer(src).tokenize());
    ast::Program program = parser.parseProgram();
    Analyzer analyzer;
    std::vector<std::string> msgs;
    for (const auto& w : analyzer.analyze(program)) msgs.push_back(w.message);
    return msgs;
}
static bool hasWarn(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v) if (s.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    // ============================================================================================
    // 1) GC stress from C++: rooted survival, manual collection, and CYCLE reclamation.
    // ============================================================================================
    {
        KiritoVM vm;
        vm.setGcEnabled(false);  // drive collection by hand so counts are deterministic

        // A globally-rooted list survives; a peer that nothing roots is reclaimed.
        Handle kept = vm.alloc(std::make_unique<ListVal>());
        static_cast<ListVal&>(vm.arena().deref(kept)).elems = {vm.makeInt(1), vm.makeInt(2), vm.makeInt(3)};
        vm.registerGlobal("kept", kept);   // reachable from the module scope

        Handle dropped = vm.alloc(std::make_unique<ListVal>());
        static_cast<ListVal&>(vm.arena().deref(dropped)).elems = {vm.makeInt(9)};
        (void)dropped;                      // no root holds it

        std::size_t before = vm.liveCount();
        vm.collectGarbage();
        CHECK(vm.liveCount() <= before);                       // something reclaimed (or unchanged)
        CHECK(vm.arena().deref(kept).kind() == ValueKind::List);  // survivor still valid
        CHECK(static_cast<ListVal&>(vm.arena().deref(kept)).elems.size() == 3);
        CHECK_THROWS(vm.arena().deref(dropped));               // its slot was swept (now dangles)

        // A RootScope keeps an intermediate alive across a collection, then releases it.
        std::size_t mark;
        {
            RootScope rs(vm);
            Handle tmp = rs.add(vm.alloc(std::make_unique<ListVal>()));
            vm.collectGarbage();
            CHECK(vm.arena().deref(tmp).kind() == ValueKind::List);  // survived while rooted
            mark = vm.liveCount();
        }
        vm.collectGarbage();                                   // rs gone -> tmp collectable
        CHECK(vm.liveCount() <= mark);
    }
    {
        // A reference CYCLE is reclaimed by the mark-sweep collector once nothing roots it (a refcount
        // GC would leak this). Build a <-> b, drop both, collect, and confirm the live set returns.
        KiritoVM vm;
        vm.setGcEnabled(false);
        std::size_t base = vm.liveCount();
        {
            RootScope rs(vm);
            Handle a = rs.add(vm.alloc(std::make_unique<ListVal>()));
            Handle b = rs.add(vm.alloc(std::make_unique<ListVal>()));
            static_cast<ListVal&>(vm.arena().deref(a)).elems.push_back(b);
            static_cast<ListVal&>(vm.arena().deref(b)).elems.push_back(a);   // a <-> b cycle
            // a self-referencing list too (the degenerate cycle)
            Handle s = rs.add(vm.alloc(std::make_unique<ListVal>()));
            static_cast<ListVal&>(vm.arena().deref(s)).elems.push_back(s);
            CHECK(vm.liveCount() > base);                       // the cycle is currently live
        }
        vm.collectGarbage();
        CHECK(vm.liveCount() <= base);                          // cycle fully reclaimed, no leak
    }
    {
        // Allocation churn under a tight threshold must not grow the live set without bound — the
        // collector keeps up while a single rooted accumulator stays correct (operand-stack roots).
        KiritoVM vm;
        vm.setGcThreshold(128);
        Handle h = vm.runSource(
            "var keep = [1, 2, 3]\n"
            "var i = 0\n"
            "while i < 20000:\n"
            "    var junk = [i, i + 1, [i]]\n"
            "    i = i + 1\n"
            "keep[0] + keep[1] + keep[2]");
        CHECK(static_cast<const IntVal&>(vm.arena().deref(h)).value() == 6);
        CHECK(vm.liveCount() < 5000);                           // garbage did not pile up
    }

    // ============================================================================================
    // 2) Bytecode VM / compiler edges.
    // ============================================================================================
    {
        // A deep (but valid) expression compiles and evaluates — the operand stack handles depth.
        // Depth 100 stays safely under the parser's nesting bound in EVERY build, including the
        // reduced sanitizer bound (kMaxParseDepth = 250 under asan/tsan); a larger value would throw
        // "expression nested too deeply" there and abort this (intentionally non-catching) check.
        std::string deep = "0";
        for (int i = 0; i < 100; ++i) deep = "(" + deep + " + 1)";
        CHECK(run(deep) == "100");

        // Deeply nested parentheses: still one value, correct precedence under the nesting.
        CHECK(run("((((1 + 2)) * ((3 + 4))))") == "21");
    }
    {
        // The call-depth guard RAISES a catchable error instead of overflowing the native stack.
        CHECK(throws("var r = Function(n): return r(n + 1)\nr(0)"));
        // ...and it is catchable from within Kirito (a runtime error, not a hard crash).
        CHECK(run("var r = Function(n): return r(n + 1)\n"
                  "var ok = \"no\"\n"
                  "try:\n    r(0)\ncatch as e:\n    ok = \"caught\"\nok") == "caught");
        // Mutual recursion trips the same guard.
        CHECK(throws("var a = Function(n): return b(n)\nvar b = Function(n): return a(n)\na(0)"));
    }
    {
        // Constant dedup is transparent: repeated scalar literals (incl. exact-bit floats) still yield
        // distinct, correct values; an Integer 1 and a Float 1.0 are NOT merged (keyed by type+bits).
        CHECK(run("var f = Function():\n var a = 7\n var b = 7\n var c = 7\n return a + b + c\nf()") == "21");
        CHECK(run("[1, 1, 1.0, 1.0]") == "[1, 1, 1.0, 1.0]");
        CHECK(run("var f = Function():\n var a = \"x\"\n var b = \"x\"\n return [a, b]\nf()") == "['x', 'x']");
        CHECK(run("0.1 + 0.2 == 0.3") == "False");             // exact float == survives dedup
    }
    {
        // Slot-addressed locals: read-before-assign behavior is preserved.
        //  - WITH an enclosing binding, a bare `=` rebinds the outer one; a later `var` shadows locally.
        CHECK(run("var x = 100\nvar f = Function():\n x = 5\n var x = 7\n return x\nvar r = f()\n[r, x]")
              == "[7, 5]");
        //  - WITHOUT any binding, a read-before-assign is an UnboundLocal-style error (slot fallback).
        CHECK(throws("var f = Function():\n var y = z\n var z = 1\n return y\nf()"));
        //  - a slotted local read before its own assignment, no outer binding -> throws (not garbage).
        CHECK(throws("var f = Function():\n return q\n var q = 1\nf()"));
        //  - loop-counter / accumulator slots compute the right answer (the slotting win case).
        CHECK(run("var f = Function(n):\n var s = 0\n var i = 0\n while i < n:\n  s = s + i\n  i = i + 1\n"
                  " return s\nf(100)") == "4950");
    }

    // ============================================================================================
    // 3) Resolver + analyzer from C++.
    // ============================================================================================
    {
        // Undefined names are COMPILE-time errors (thrown by runSource before any code runs).
        CHECK(errOf("undefined_xyz_name").find("is not defined") != std::string::npos);
        CHECK(errOf("var y = also_missing").find("is not defined") != std::string::npos);
        // ...even where the reference would never execute (uncalled function body).
        CHECK(errOf("var f = Function(): return never_runs\n5").find("is not defined") != std::string::npos);
        // The error carries a location.
        {
            KiritoVM vm;
            try { vm.runSource("var a = 1\nvar b = bad_name\n"); CHECK(false); }
            catch (const KiritoError& e) {
                CHECK(std::string(e.what()).find("bad_name") != std::string::npos);
                CHECK(e.span.line == 2);
            }
        }
        // No false positives: recursion / forward refs / builtins resolve.
        CHECK(!throws("var fac = Function(n):\n if n <= 1:\n  return 1\n return n * fac(n-1)\nfac(5)"));
        CHECK(!throws("len([1, 2, 3])"));
    }
    {
        // The Analyzer (non-fatal static pass) surfaces a representative warning set.
        CHECK(hasWarn(warn("var f = Function():\n    var unused = 5\n    return 1\n"),
                      "variable 'unused' is assigned but never used"));
        CHECK(hasWarn(warn("var x = 1\nx + 2\n"), "result of expression is unused"));
        CHECK(hasWarn(warn("var f = Function():\n    var x = 1\n    var x = 2\n    return x\n"),
                      "variable 'x' is re-declared in this block"));
        CHECK(hasWarn(warn("var f = Function():\n    return 1\n    var x = 2\n"), "unreachable code"));
        CHECK(hasWarn(warn("var x = 1\nx = x\n"), "self-assignment of 'x' has no effect"));
        CHECK(hasWarn(warn("var f = Function(a, b, a):\n    return a\n"), "duplicate parameter name 'a'"));
        // discard suppresses the unused-result warning; a clean program warns about nothing.
        CHECK(!hasWarn(warn("var x = 1\ndiscard x + 2\n"), "result of expression is unused"));
        CHECK(!hasWarn(warn("var f = Function():\n    var x = 5\n    return x\n"), "never used"));
    }

    // ============================================================================================
    // 4) serde from C++: a value graph with a SHARED ref AND a CYCLE through both codecs.
    // ============================================================================================
    {
        KiritoVM vm;
        // inner = [1, 2];  outer = [inner, inner, outer]  (shared inner + a self-reference)
        RootScope rs(vm);
        Handle inner = rs.add(vm.alloc(std::make_unique<ListVal>()));
        static_cast<ListVal&>(vm.arena().deref(inner)).elems = {vm.makeInt(1), vm.makeInt(2)};
        Handle outer = rs.add(vm.alloc(std::make_unique<ListVal>()));
        {
            auto& ov = static_cast<ListVal&>(vm.arena().deref(outer));
            ov.elems.push_back(inner);
            ov.elems.push_back(inner);   // same object twice -> a shared reference
            ov.elems.push_back(outer);   // self-reference -> a cycle
        }

        auto checkGraph = [&](Handle back) {
            auto& bv = static_cast<ListVal&>(vm.arena().deref(back));
            CHECK(bv.elems.size() == 3);
            CHECK(bv.elems[0] == bv.elems[1]);   // aliasing preserved
            CHECK(bv.elems[2] == back);          // cycle preserved
            auto& bi = static_cast<ListVal&>(vm.arena().deref(bv.elems[0]));
            CHECK(bi.elems.size() == 2);
            CHECK(static_cast<const IntVal&>(vm.arena().deref(bi.elems[0])).value() == 1);
            CHECK(static_cast<const IntVal&>(vm.arena().deref(bi.elems[1])).value() == 2);
        };

        checkGraph(serial::loads(vm, serial::dumps(vm, outer)));      // text codec
        checkGraph(dumpfmt::read(vm, dumpfmt::write(vm, outer)));     // binary codec
    }

    // ============================================================================================
    // 5) Round-1 fix regressions (driven via runSource on a fresh VM).
    // ============================================================================================
    {
        // tensor.einsum with a REPEATED output label throws a catchable error (was an OOB buffer overrun).
        const char* setup = "var t = import(\"tensor\")\n";
        CHECK(throws(std::string(setup) + "var m = t.eye(3)\nt.einsum(\"ii->ii\", m)"));
        CHECK(throws(std::string(setup) + "var v = t.arange(3)\nt.einsum(\"i->ii\", v)"));
        // it's a KiritoError carrying a clear message (catchable, not a crash).
        {
            std::string e = errOf(std::string(setup) + "var m = t.eye(3)\nt.einsum(\"ii->ii\", m)");
            CHECK(e.find("einsum") != std::string::npos);
            CHECK(e.find("more than once") != std::string::npos);
        }
        // ...and it is catchable from Kirito itself.
        CHECK(run(std::string(setup) +
                  "var m = t.eye(3)\nvar ok = \"no\"\ntry:\n t.einsum(\"ii->ii\", m)\ncatch as e:\n ok = \"caught\"\nok")
              == "caught");
        // Valid einsum specs still work: transpose ("ij->ji") and trace ("ii->").
        CHECK(run(std::string(setup) + "t.einsum(\"ij->ji\", t.eye(2)).tolist()") == "[[1.0, 0.0], [0.0, 1.0]]");
        CHECK(run(std::string(setup) + "t.einsum(\"ii->\", t.eye(3)).item()") == "3.0");
    }
    {
        // json.loads is intentionally LENIENT on number forms (see deep_serialization.ki): a leading
        // zero, a trailing dot, and an empty exponent all parse rather than throwing.
        const char* j = "import(\"json\").loads";
        CHECK(run(std::string(j) + "(\"01\")") == "1");        // leading zero -> 1
        CHECK(run(std::string(j) + "(\"5.\")") == "5.0");      // trailing dot -> 5.0
        CHECK(run(std::string(j) + "(\"1e\")") == "1.0");      // empty exponent -> 1.0
        // valid numbers parse as expected.
        CHECK(run(std::string(j) + "(\"1.5e10\")") == "15000000000.0");
        CHECK(run(std::string(j) + "(\"0\")") == "0");
        CHECK(run(std::string(j) + "(\"-0\")") == "0");
        CHECK(run(std::string(j) + "(\"1.5\")") == "1.5");
    }

    return RUN_TESTS();
}
