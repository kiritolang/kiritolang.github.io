// Adversarial / edge-case probing: pathological inputs must never crash the host, corrupt memory,
// or invoke undefined behaviour. Each case must either compute a well-defined value or throw a
// clean, catchable KiritoError. (Run this under -fsanitize=address,undefined to catch UB.)
#include <limits>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
// Run and return the error message (empty if it did not throw).
static std::string err(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return ""; }
    catch (const KiritoError& e) { return e.what(); }
}

int main() {
    // --- integer overflow is WELL-DEFINED two's-complement wraparound, not UB ----------------
    {
        KiritoVM vm;
        const std::string MAX = "9223372036854775807";        // INT64_MAX
        const std::string MIN = "(-9223372036854775807 - 1)"; // INT64_MIN (no literal for it)
        CHECK(evalStr(vm, MAX + " + 1") == "-9223372036854775808");   // wraps to INT64_MIN
        CHECK(evalStr(vm, MAX + " * 2") == "-2");
        CHECK(evalStr(vm, MIN + " - 1") == "9223372036854775807");    // wraps to INT64_MAX
        CHECK(evalStr(vm, "-" + MIN) == "-9223372036854775808");      // -INT64_MIN wraps
        CHECK(evalStr(vm, MIN + " // -1") == "-9223372036854775808"); // would trap on hardware
        CHECK(evalStr(vm, MIN + " % -1") == "0");
        CHECK(evalStr(vm, "2 ** 200") == "0");                         // pow overflows cleanly
        CHECK(evalStr(vm, "2 ** 63") == "-9223372036854775808");
    }

    // --- division / modulo by zero throw (never crash) ---------------------------------------
    {
        KiritoVM vm;
        CHECK(err(vm, "1 // 0").find("division by zero") != std::string::npos);
        CHECK(err(vm, "1 % 0").find("modulo by zero") != std::string::npos);
        CHECK(err(vm, "1 / 0").find("division by zero") != std::string::npos);
        CHECK(err(vm, "1.0 / 0.0").find("division by zero") != std::string::npos);
        CHECK(err(vm, "1.0 // 0.0").find("division by zero") != std::string::npos);
    }

    // --- unbounded recursion is caught as a catchable error, not a stack overflow -------------
    {
        KiritoVM vm;
        vm.setMaxCallDepth(200);  // keep the native recursion shallow (asan-safe) but still prove the guard
        CHECK(err(vm, "var f = Function(n):\n    return f(n + 1)\nf(0)\n")
                  .find("maximum recursion depth") != std::string::npos);
        // mutual recursion too
        CHECK(err(vm,
            "var a = Function(n):\n    return b(n)\n"
            "var b = Function(n):\n    return a(n)\na(0)\n")
                  .find("maximum recursion depth") != std::string::npos);
        // the VM survives and keeps working afterwards
        CHECK(evalStr(vm, "1 + 1") == "2");
    }

    // --- self-referential containers: stringify and equals terminate (no infinite loop) -------
    {
        KiritoVM vm;
        CHECK(evalStr(vm, "var a = [1]\na.append(a)\nString(a)") == "[1, [...]]");
        CHECK(evalStr(vm, "var a = [1]\na.append(a)\na == a") == "True");
        CHECK(evalStr(vm, "var d = {}\nd[\"k\"] = d\nString(d)") == "{'k': {...}}");
        // a cyclic structure is unhashable rather than looping forever
        CHECK(!err(vm, "var a = []\na.append(a)\nvar d = {}\nd[a] = 1\n").empty());
    }

    // --- mutable values are unhashable: clean error, never a crash ----------------------------
    {
        KiritoVM vm;
        CHECK(!err(vm, "var d = {}\nd[[1, 2]] = 3").empty());
        CHECK(!err(vm, "var d = {}\nd[{1: 2}] = 3").empty());
    }

    // --- slicing edge cases are total (any indices, any step) ---------------------------------
    {
        KiritoVM vm;
        CHECK(evalStr(vm, "var a = [1, 2, 3, 4, 5]\nString(a[10:20])") == "[]");
        CHECK(evalStr(vm, "var a = [1, 2, 3, 4, 5]\nString(a[-100:100])") == "[1, 2, 3, 4, 5]");
        CHECK(evalStr(vm, "var a = [1, 2, 3, 4, 5]\nString(a[::-1])") == "[5, 4, 3, 2, 1]");
        CHECK(evalStr(vm, "var a = [1, 2, 3, 4, 5]\nString(a[::2])") == "[1, 3, 5]");
        CHECK(err(vm, "var a = [1]\na[0:5:0]").find("slice step") != std::string::npos);  // zero step rejected
    }

    // --- Unicode is code-point indexed, never byte-indexed (UTF-8 literals below) -------------
    {
        KiritoVM vm;
        CHECK(evalStr(vm, "len(\"héllo→世界\")") == "8");          // 8 code points, 14 bytes
        CHECK(evalStr(vm, "\"héllo→世界\"[6]") == "世");
        CHECK(evalStr(vm, "len(\"😀\")") == "1");                  // astral plane = one code point
        CHECK(evalStr(vm, "\"abc世\"[::-1]") == "世cba");
    }

    // --- empty-sequence reductions throw rather than read out of bounds -----------------------
    {
        KiritoVM vm;
        CHECK(!err(vm, "min([])").empty());
        CHECK(!err(vm, "max([])").empty());
        CHECK(evalStr(vm, "sum([])") == "0");  // sum of empty is defined (0)
        CHECK(err(vm, "[][0]").find("index out of range") != std::string::npos);
        CHECK(err(vm, "{}[\"missing\"]").find("key not found") != std::string::npos);
    }

    // --- string repetition: negative/zero counts give empty, large counts don't overflow ------
    {
        KiritoVM vm;
        CHECK(evalStr(vm, "\"ab\" * 0") == "");
        CHECK(evalStr(vm, "\"ab\" * -5") == "");
        CHECK(evalStr(vm, "len(\"ab\" * 100000)") == "200000");
    }

    // --- type mismatches in operators throw with an actionable, op-appropriate message --------
    {
        KiritoVM vm;
        CHECK(err(vm, "1 + \"a\"").find("for arithmetic") != std::string::npos);
        CHECK(err(vm, "1 < \"a\"").find("for comparison") != std::string::npos);  // not "arithmetic"
        CHECK(err(vm, "None()").find("not callable") != std::string::npos);
        CHECK(err(vm, "None.foo").find("has no attribute") != std::string::npos);
        CHECK(err(vm, "\"abc\"[10]").find("index out of range") != std::string::npos);
    }

    // --- deeply nested structures build and tear down without crashing (GC reachability) ------
    {
        KiritoVM vm;
        CHECK(evalStr(vm,
            "var b = []\n"
            "var i = 0\n"
            "while i < 20000:\n"
            "    var c = [b]\n"
            "    b = c\n"
            "    i = i + 1\n"
            "len(b)") == "1");
    }

    // --- equality across cyclic structures of equal shape terminates --------------------------
    {
        KiritoVM vm;
        CHECK(!err(vm,
            "var a = []\na.append(a)\n"
            "var b = []\nb.append(b)\n"
            "a == b\n").empty() || true);  // must not hang/crash; result either way is acceptable
    }

    return RUN_TESTS();
}
