// Round-3 (R6) internals + regression coverage, pinned from C++ where the C++ side is the real
// contract — these pin the bugs found/fixed in the R6 audit, at a precision the pure-Kirito .ki
// tests can't reach. Five fix areas:
//
//   1. Integer<->Float EXACT comparison (runtime.hpp: compareIntFloat / numericCompare /
//      intFloatEqual + FloatVal::hash). ==/!=/<,<=,>,>= must be exact past 2^53 (no lossy
//      (double)int round-trip): 2^53+1 != the float 2^53; INT64_MAX != 2^63.0; 1 == 1.0 AND they
//      hash equal; ordering exact at the boundary; a NaN operand is UNORDERED. Tested at the C++
//      level (construct values, compare the helpers + hash) AND through runSource (the operators).
//   2. tensor autograd backward is ITERATIVE (stdlib_tensor.hpp runBackward): a deep graph (a long
//      unrolled chain) must NOT overflow the native C++ stack — backward completes and the grad is
//      correct. Driven via runSource (a few-thousand-deep `y = y + x` chain).
//   3. makeMethod zero-arg OOB guards (stdlib_io.hpp BytesIO.write; stdlib_time.hpp DateTime.format
//      / _setstate_): calling these with zero arguments must throw a catchable KiritoError, not read
//      a[0] out of bounds (UB). Driven via runSource.
//   4. regex hex-escape range (regex_engine.hpp readHex): `\U00110000` and an above-range value
//      throw; valid `\x`/`\u`/`\U` escapes still match.
//   5. complex factory negative-dim guards (stdlib_complex.hpp): complex.zeros / ones / identity
//      with a negative dimension throw (instead of a huge/garbage allocation).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// A local "must NOT throw" assertion (check.hpp ships CHECK + CHECK_THROWS, not its inverse).
#define CHECK_NO_THROW(expr)                                                     \
    do {                                                                         \
        try { (void)(expr); }                                                    \
        catch (...) {                                                            \
            std::printf("FAIL %s:%d  unexpected exception: %s\n",                \
                        __FILE__, __LINE__, #expr);                              \
            ++kitest::failures;                                                  \
        }                                                                        \
    } while (0)

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

int main() {
    // ============================================================================================
    // 1) Integer<->Float EXACT comparison — pinned at the C++ level (the helpers + hash) AND through
    //    the operators (runSource). The whole point: NO lossy (double)int round-trip, so magnitudes
    //    beyond 2^53 stay distinct and equality agrees with ordering and with hashing.
    // ============================================================================================
    {
        // --- the three-way helper compareIntFloat(int64, double): -1/0/+1, or 2 for UNORDERED ---
        constexpr int64_t kI53   = (int64_t(1) << 53);          // 2^53, exactly representable as double
        const double      kF53   = 9007199254740992.0;          // 2^53 as a double
        // 2^53 itself: int == float.
        CHECK(compareIntFloat(kI53, kF53) == 0);
        CHECK(intFloatEqual(kI53, kF53));
        // 2^53 + 1: the int is distinct from the float 2^53 (the lossy round-trip would say equal).
        CHECK(compareIntFloat(kI53 + 1, kF53) == 1);            // (2^53 + 1) > 2^53
        CHECK(!intFloatEqual(kI53 + 1, kF53));
        // (float 2^53) vs (int 2^53 - 1): the int is smaller.
        CHECK(compareIntFloat(kI53 - 1, kF53) == -1);

        // INT64_MAX (2^63 - 1) is NOT equal to the double 2^63 (which is the smallest double >= 2^63).
        const int64_t kMax   = std::numeric_limits<int64_t>::max();   // 2^63 - 1
        const double  kTwo63 = 9223372036854775808.0;                 // 2^63 exactly as a double
        CHECK(compareIntFloat(kMax, kTwo63) == -1);             // (2^63 - 1) < 2^63
        CHECK(!intFloatEqual(kMax, kTwo63));
        // ...and any int64 is < 2^63, while INT64_MIN == -2^63 exactly.
        const int64_t kMin = std::numeric_limits<int64_t>::min();     // -2^63
        CHECK(compareIntFloat(kMin, -kTwo63) == 0);            // INT64_MIN == -2^63.0
        CHECK(intFloatEqual(kMin, -kTwo63));
        CHECK(compareIntFloat(kMin, kTwo63) == -1);           // every int64 < 2^63
        CHECK(compareIntFloat(kMax, -kTwo63) == 1);           // every int64 > -2^63 - 1

        // A fractional float orders strictly between the floors it brackets.
        CHECK(compareIntFloat(2, 2.5) == -1);                  // 2 < 2.5
        CHECK(compareIntFloat(3, 2.5) == 1);                   // 3 > 2.5
        CHECK(compareIntFloat(0, -0.0) == 0);                  // 0 == -0.0

        // A NaN operand is UNORDERED (sentinel 2) — neither <, ==, nor >.
        const double nan = std::numeric_limits<double>::quiet_NaN();
        CHECK(compareIntFloat(0, nan) == 2);
        CHECK(compareIntFloat(kMax, nan) == 2);
        CHECK(!intFloatEqual(0, nan));

        // +inf is above every int64, -inf below every int64 (never equal, never UNORDERED).
        const double inf = std::numeric_limits<double>::infinity();
        CHECK(compareIntFloat(kMax, inf) == -1);
        CHECK(compareIntFloat(kMin, -inf) == 1);
        CHECK(compareIntFloat(0, inf) == -1);
        CHECK(compareIntFloat(0, -inf) == 1);
    }
    {
        // --- numericCompare(const Object&, const Object&): the Object-level dispatch, both operand
        //     orders, used by the `<,<=,>,>=` operators and kiLessThan/sort ---
        KiritoVM vm;
        Handle i53p1 = vm.makeInt((int64_t(1) << 53) + 1);
        Handle f53   = vm.makeFloat(9007199254740992.0);       // 2^53
        const Object& I = vm.arena().deref(i53p1);
        const Object& F = vm.arena().deref(f53);
        CHECK(numericCompare(I, F) == 1);                      // (2^53 + 1) > 2^53  (Integer, Float)
        CHECK(numericCompare(F, I) == -1);                     // 2^53 < (2^53 + 1)  (Float, Integer) — sense flipped
        // Integer/Integer.
        Handle a = vm.makeInt(5), b = vm.makeInt(7);
        CHECK(numericCompare(vm.arena().deref(a), vm.arena().deref(b)) == -1);
        CHECK(numericCompare(vm.arena().deref(b), vm.arena().deref(a)) == 1);
        CHECK(numericCompare(vm.arena().deref(a), vm.arena().deref(a)) == 0);
        // Float/Float exact.
        Handle p = vm.makeFloat(1.5), q = vm.makeFloat(1.5);
        CHECK(numericCompare(vm.arena().deref(p), vm.arena().deref(q)) == 0);
        // A NaN operand stays UNORDERED in either position.
        Handle nanH = vm.makeFloat(std::numeric_limits<double>::quiet_NaN());
        CHECK(numericCompare(vm.arena().deref(nanH), vm.arena().deref(a)) == 2);
        CHECK(numericCompare(vm.arena().deref(a), vm.arena().deref(nanH)) == 2);
    }
    {
        // --- IntVal::equals / FloatVal::equals are symmetric and EXACT across the boundary ---
        KiritoVM vm;
        Handle i1 = vm.makeInt(1), f1 = vm.makeFloat(1.0);
        const Object& I1 = vm.arena().deref(i1);
        const Object& F1 = vm.arena().deref(f1);
        CHECK(I1.equals(vm.arena(), F1));                       // 1 == 1.0
        CHECK(F1.equals(vm.arena(), I1));                       // 1.0 == 1  (symmetric)
        // 2^53 + 1 vs the float 2^53: NOT equal, both directions.
        Handle bigI = vm.makeInt((int64_t(1) << 53) + 1);
        Handle bigF = vm.makeFloat(9007199254740992.0);
        CHECK(!vm.arena().deref(bigI).equals(vm.arena(), vm.arena().deref(bigF)));
        CHECK(!vm.arena().deref(bigF).equals(vm.arena(), vm.arena().deref(bigI)));
        // kiEquals (the operator path) agrees.
        CHECK(kiEquals(vm, i1, f1));
        CHECK(!kiEquals(vm, bigI, bigF));
    }
    {
        // --- FloatVal::hash maps an integral float in [-2^63, 2^63) to the int hash, so `==` agrees
        //     with hashing (a Set/Dict key works) — distinct-but-close floats stay distinct keys ---
        KiritoVM vm;
        auto H = [&](Handle h) { return vm.arena().deref(h).hash(); };
        // 1 and 1.0 hash equal.
        CHECK(H(vm.makeInt(1)) == H(vm.makeFloat(1.0)));
        // 0 and 0.0 (and -0.0) hash equal.
        CHECK(H(vm.makeInt(0)) == H(vm.makeFloat(0.0)));
        CHECK(H(vm.makeInt(0)) == H(vm.makeFloat(-0.0)));
        // a large exact integral float matches its Integer.
        CHECK(H(vm.makeInt(int64_t(1) << 53)) == H(vm.makeFloat(9007199254740992.0)));
        CHECK(H(vm.makeInt(-12345678)) == H(vm.makeFloat(-12345678.0)));
        // a NON-integral float must hash consistently with itself.
        CHECK(H(vm.makeFloat(2.5)) == H(vm.makeFloat(2.5)));
        // out-of-range / non-finite integral cases fall through to the double hash without crashing.
        CHECK_NO_THROW(H(vm.makeFloat(std::numeric_limits<double>::infinity())));
        CHECK_NO_THROW(H(vm.makeFloat(std::numeric_limits<double>::quiet_NaN())));
        // 2^63 (out of the [-2^63, 2^63) range) is integral but must NOT map to an int hash; it just
        // hashes as a double, consistently.
        CHECK(H(vm.makeFloat(9223372036854775808.0)) == H(vm.makeFloat(9223372036854775808.0)));
    }
    {
        // --- the OPERATORS through runSource: ==/!= exact, ordering exact, NaN unordered ---
        // 2^53 + 1 != the float 2^53 (the headline case).
        CHECK(run("9007199254740993 == 9007199254740992.0") == "False");
        CHECK(run("9007199254740993 != 9007199254740992.0") == "True");
        CHECK(run("9007199254740993 > 9007199254740992.0") == "True");
        CHECK(run("9007199254740992 == 9007199254740992.0") == "True");   // 2^53 itself: equal
        // INT64_MAX (2^63 - 1) != 2^63.0, and is < it.
        CHECK(run("9223372036854775807 == 9.223372036854776e18") == "False");
        CHECK(run("9223372036854775807 < 9.223372036854776e18") == "True");
        // 1 == 1.0 and the small-value sanity.
        CHECK(run("1 == 1.0") == "True");
        CHECK(run("1.0 == 1") == "True");
        CHECK(run("2 < 2.5 and 3 > 2.5") == "True");
        // == agrees with hashing inside a Set/Dict: 1 and 1.0 collapse to one key.
        CHECK(run("len({1, 1.0, 1})") == "1");
        CHECK(run("var d = {}\nd[1] = \"a\"\nd[1.0] = \"b\"\nlen(d)") == "1");
        CHECK(run("var d = {}\nd[1] = \"a\"\nd[1.0] = \"b\"\nd[1]") == "b");  // 1.0 overwrote key 1
        // ...but 2^53 and 2^53+1 are DISTINCT keys (they're not equal, so they don't collide).
        CHECK(run("len({9007199254740992.0, 9007199254740993})") == "2");
        // NaN is unordered: every comparison false, and NaN != NaN.
        CHECK(run("var nan = import(\"math\").nan\nnan == 1") == "False");
        CHECK(run("var nan = import(\"math\").nan\nnan < 1") == "False");
        CHECK(run("var nan = import(\"math\").nan\nnan > 1") == "False");
        CHECK(run("var nan = import(\"math\").nan\nnan != 1") == "True");
        CHECK(run("var nan = import(\"math\").nan\nnan == nan") == "False");
        // exact float == is still exact (the boundary rule: ONLY ==/!= are exact, no tolerance).
        CHECK(run("0.1 + 0.2 == 0.3") == "False");
        // sort/min/max use the exact compare too: a List mixing the big int + close float orders right
        // (the float 2^53 < the int 2^53+1; the float renders as 9.00719925474099e+15).
        CHECK(run("sorted([9007199254740993, 9007199254740992.0])")
              == "[9.00719925474099e+15, 9007199254740993]");
        CHECK(run("max([9007199254740993, 9007199254740992.0])") == "9007199254740993");
        CHECK(run("min([9007199254740993, 9007199254740992.0])") == "9.00719925474099e+15");
    }

    // ============================================================================================
    // 2) tensor autograd backward is ITERATIVE — a deep graph must not overflow the native stack.
    //    Build a long unrolled chain `y = y + x` (depth d): then y = y0 + d*x, so dy/dx = d, and the
    //    sum's backward seeds 1, giving x.grad == d. Completing at all is the stack-safety contract.
    // ============================================================================================
    {
        // A modest-but-deep chain (3000) — enough to blow a recursive backward, cheap to run.
        const char* prog =
            "var t = import(\"tensor\")\n"
            "var x = t.full([1], 2.0, requiresgrad = True)\n"
            "var y = t.zeros([1])\n"
            "var i = 0\n"
            "while i < 3000:\n"
            "    y = y + x\n"
            "    i = i + 1\n"
            "y.sum().backward()\n"
            "x.grad.item()";
        // y = 3000*x, so dy/dx = 3000 (summed over the 1-element tensor).
        CHECK(run(prog) == "3000.0");
    }
    {
        // A second shape: a multiplicative-then-additive chain, still deep, grad still exact.
        // y starts at x; each step y = y + x*1.0  => after d steps y = (d+1)*x => grad = d+1.
        const char* prog =
            "var t = import(\"tensor\")\n"
            "var x = t.full([2], 1.0, requiresgrad = True)\n"
            "var y = x\n"
            "var i = 0\n"
            "while i < 2000:\n"
            "    y = y + x\n"
            "    i = i + 1\n"
            "y.sum().backward()\n"
            "x.grad.tolist()";
        // 2001 contributions to each of the 2 elements.
        CHECK(run(prog) == "[2001.0, 2001.0]");
    }
    {
        // The deep chain must not CRASH; even wrapped in try it should simply succeed (no throw).
        CHECK(!throws(
            "var t = import(\"tensor\")\n"
            "var x = t.full([1], 1.0, requiresgrad = True)\n"
            "var y = t.zeros([1])\n"
            "var i = 0\n"
            "while i < 4000:\n"
            "    y = y + x\n"
            "    i = i + 1\n"
            "y.sum().backward()\n"));
    }

    // ============================================================================================
    // 3) makeMethod zero-arg OOB guards — calling a 1-arg native method with NO args must throw a
    //    catchable KiritoError (it used to read a[0] out of bounds = UB).
    // ============================================================================================
    {
        // BytesIO.write() with no argument throws.
        CHECK(throws("var io = import(\"io\")\nvar b = io.BytesIO()\nb.write()"));
        {
            std::string e = errOf("var io = import(\"io\")\nvar b = io.BytesIO()\nb.write()");
            CHECK(e.find("write") != std::string::npos);       // message names the method
        }
        // it's catchable from Kirito (a runtime error, not a hard crash).
        CHECK(run("var io = import(\"io\")\nvar b = io.BytesIO()\nvar ok = \"no\"\n"
                  "try:\n b.write()\ncatch as e:\n ok = \"caught\"\nok") == "caught");
        // the happy path still works (and returns the byte count).
        CHECK(run("var io = import(\"io\")\nvar b = io.BytesIO()\nb.write(\"abc\")") == "3");

        // DateTime.format() with no argument throws.
        CHECK(throws("var tm = import(\"time\")\nvar d = tm.make(2020, 1, 2, 3, 4, 5)\nd.format()"));
        {
            std::string e = errOf("var tm = import(\"time\")\nvar d = tm.make(2020, 1, 2, 3, 4, 5)\nd.format()");
            CHECK(e.find("format") != std::string::npos);
        }
        // happy path: format with a spec works.
        CHECK(run("var tm = import(\"time\")\nvar d = tm.make(2020, 1, 2, 3, 4, 5)\nd.format(\"%Y\")") == "2020");

        // DateTime._setstate_() with no argument throws (the deserialization hook; OOB-guarded).
        CHECK(throws("var tm = import(\"time\")\nvar d = tm.make(2020, 1, 2, 3, 4, 5)\nd._setstate_()"));
        {
            std::string e = errOf("var tm = import(\"time\")\nvar d = tm.make(2020, 1, 2, 3, 4, 5)\nd._setstate_()");
            CHECK(e.find("_setstate_") != std::string::npos);
        }
    }

    // ============================================================================================
    // 4) regex hex-escape range — an above-U+10FFFF escape throws; valid escapes match.
    // ============================================================================================
    {
        const char* re = "var re = import(\"regex\")\n";
        // \UFFFFFFFF (8 F's = 0xFFFFFFFF) is way above U+10FFFF -> throws.
        CHECK(throws(std::string(re) + "re.compile(\"\\\\UFFFFFFFF\")"));
        // \U00110000 (one past U+10FFFF) -> throws (the exact boundary).
        CHECK(throws(std::string(re) + "re.compile(\"\\\\U00110000\")"));
        {
            std::string e = errOf(std::string(re) + "re.compile(\"\\\\U00110000\")");
            CHECK(e.find("range") != std::string::npos);       // "out of range (above U+10FFFF)"
        }
        // \U0010FFFF (exactly U+10FFFF, the max code point) is VALID.
        CHECK(!throws(std::string(re) + "re.compile(\"\\\\U0010FFFF\")"));
        // valid short escapes match: \x41 == 'A', A == 'A', \U00000041 == 'A'.
        CHECK(run(std::string(re) + "re.match(\"\\\\x41\", \"A\").group(0)") == "A");
        CHECK(run(std::string(re) + "re.match(\"\\\\u0041\", \"A\").group(0)") == "A");
        CHECK(run(std::string(re) + "re.match(\"\\\\U00000041\", \"A\").group(0)") == "A");
        // an incomplete escape (too few hex digits) throws too.
        CHECK(throws(std::string(re) + "re.compile(\"\\\\x4\")"));
    }

    // ============================================================================================
    // 5) complex factory negative-dim guards — negative dims throw (no garbage/huge allocation).
    // ============================================================================================
    {
        const char* cx = "var c = import(\"complex\")\n";
        // zeros / ones with a negative dimension throw.
        CHECK(throws(std::string(cx) + "c.zeros(-1, 3)"));
        CHECK(throws(std::string(cx) + "c.zeros(2, -3)"));
        CHECK(throws(std::string(cx) + "c.ones(-2, 2)"));
        CHECK(throws(std::string(cx) + "c.ones(2, -1)"));
        // identity with a negative dimension throws.
        CHECK(throws(std::string(cx) + "c.identity(-4)"));
        {
            std::string e = errOf(std::string(cx) + "c.zeros(-1, 3)");
            CHECK(e.find("non-negative") != std::string::npos);
        }
        // the happy path works (trace() returns a Complex; compares equal to a real when imag == 0).
        CHECK(run(std::string(cx) + "c.zeros(2, 2).trace() == 0") == "True");
        CHECK(run(std::string(cx) + "c.identity(3).trace() == 3") == "True");
        // a NON-square ones is a legal construction (negative-dim guard is about negatives, not shape).
        CHECK(!throws(std::string(cx) + "c.ones(2, 3)"));
        // zero is a legal dimension (an empty matrix), not negative — must NOT throw.
        CHECK(!throws(std::string(cx) + "c.zeros(0, 0)"));
        CHECK(!throws(std::string(cx) + "c.identity(0)"));
    }

    return RUN_TESTS();
}
