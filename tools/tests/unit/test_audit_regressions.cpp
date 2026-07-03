// Regression tests for the audit pass: each case is a previously-found crash (SIGSEGV/SIGABRT),
// undefined behaviour, silent-wrong-result, or missing-kwarg bug that must now throw a clean,
// catchable error (or compute the right value). Run under -fsanitize=address,undefined to confirm the
// UB is gone. Generated deep-nesting sources are built in-memory so no multi-MB fixtures are committed.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run `src` in a fresh stdlib-equipped VM; return the error message ("" if it did not throw).
static std::string err(const std::string& src) {
    KiritoVM vm;
    vm.installStandardLibrary();
    try { vm.runSource(src); return ""; }
    catch (const KiritoError& e) { return e.what(); }
    catch (const std::exception& e) { return std::string("std:") + e.what(); }
}
// Run `src`, return its stringified result (throws propagate — used for must-succeed cases).
static std::string ok(const std::string& src) {
    KiritoVM vm;
    vm.installStandardLibrary();
    return vm.stringify(vm.runSource(src));
}
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // === deep nesting: iterative operator/postfix chains and indentation pyramids must RAISE at
    // parse time, never overflow the native stack (parsing or the recursive AST teardown). 5000 > the
    // 2000 non-sanitizer parse-depth cap (and the 250 sanitizer cap). ----------------------------
    {
        const int N = 5000;
        std::string idx = "var a = [0]\ndiscard a";
        for (int i = 0; i < N; ++i) idx += "[0]";
        CHECK(has(err(idx), "nested too deeply"));

        std::string add = "discard ";
        for (int i = 0; i < N; ++i) add += "1+";
        add += "1";
        CHECK(has(err(add), "nested too deeply"));

        std::string pw = "discard ";          // ** is right-recursive: was entirely unguarded
        for (int i = 0; i < N; ++i) pw += "1**";
        pw += "1";
        CHECK(has(err(pw), "nested too deeply"));

        std::string mem = "var a = 0\ndiscard a";
        for (int i = 0; i < N; ++i) mem += ".b";
        CHECK(has(err(mem), "nested too deeply"));

        std::string blk;                       // nested-block pyramid: parseIndentedSuite recursion
        for (int i = 0; i < N; ++i) blk += std::string(static_cast<std::size_t>(i), ' ') + "if True:\n";
        blk += std::string(static_cast<std::size_t>(N), ' ') + "discard 1\n";
        CHECK(has(err(blk), "nested too deeply"));

        // a non-pathological chain still parses and runs fine (the guard does not over-trigger)
        CHECK(ok("1+2+3+4+5+6+7+8+9+10") == "55");
        CHECK(ok("var a = [[1, 2], [3, 4]]\na[0][1] + a[1][0]") == "5");
    }

    // === regex: a start pos past the end must not read out of bounds (zero-width \b assertion). ===
    {
        CHECK(err("import(\"regex\").compile(r\"\\b\").search(\"ab\", 1000000)") == "");      // no crash
        CHECK(err("import(\"regex\").compile(r\"\\b\").match(\"ab\", 1000000)") == "");
        CHECK(err("import(\"regex\").compile(r\"\\b\").fullmatch(\"ab\", 1000000)") == "");
    }

    // === tensor: size-overflow and empty-axis and bad-downcast crashes now throw -----------------
    {
        CHECK(has(err("import(\"tensor\").eye(4294967296)"), "too large"));     // n*n overflowed -> OOB write
        CHECK(has(err("import(\"tensor\").eye(-1)"), "non-negative"));
        CHECK(has(err("import(\"tensor\").zeros([2, 0]).sum(1)"), "zero-size"));  // empty-axis reduction
        CHECK(has(err("import(\"tensor\").triu(7)"), "expects a Tensor"));        // was SIGABRT (bad downcast)
        CHECK(has(err("import(\"tensor\").tril(7)"), "expects a Tensor"));
        CHECK(has(err("var m = import(\"matrix\")\nimport(\"tensor\").diag(m.identity(3))"), "expects a Tensor"));
    }

    // === native-method arity: under-supplied required args throw cleanly (no OOB span read / UB) ==
    {
        CHECK(has(err("[1, 2, 3].insert()"), "insert"));                         // was OOB read + corruption
        CHECK(has(err("[1, 2, 3].insert(\"x\", 9)"), "Integer"));                // was static_cast<IntVal> UB
        CHECK(has(err("[1, 2, 3].remove()"), "remove"));
        CHECK(has(err("{\"a\": 1}.pop()"), "pop"));
        CHECK(has(err("{1, 2}.discard()"), "discard"));
        CHECK(has(err("var r = import(\"random\").Random(1)\ndiscard r.sample([1, 2, 3])"), "sample"));  // was silent wrong result
        CHECK(has(err("var r = import(\"random\").Random(1)\ndiscard r.uniform(1)"), "uniform"));
        CHECK(has(err("var r = import(\"random\").Random(1)\ndiscard r.randint(1)"), "randint"));
        // insert with the right arity + types still works
        CHECK(ok("var xs = [1, 3]\nxs.insert(1, 2)\nxs") == "[1, 2, 3]");
    }

    // === keyword arguments now accepted by the arity-overloaded tensor.arange / matrix.Matrix -----
    {
        CHECK(ok("import(\"tensor\").arange(stop = 4).tolist()") == "[0.0, 1.0, 2.0, 3.0]");
        CHECK(ok("import(\"tensor\").arange(start = 1, stop = 4).tolist()") == "[1.0, 2.0, 3.0]");
        CHECK(ok("import(\"tensor\").arange(3).tolist()") == "[0.0, 1.0, 2.0]");                 // positional overload intact
        CHECK(ok("var m = import(\"matrix\").Matrix(rows = 2, cols = 2)\nString(m[0, 0])") == "0.0");
        CHECK(ok("var m = import(\"matrix\").Matrix([[1, 2], [3, 4]])\nString(m[1, 1])") == "4.0");  // nested-list overload intact
        CHECK(has(err("import(\"tensor\").arange(stop = 4, bogus = 1)"), "unexpected keyword"));
    }

    // === semver: a malformed range satisfies nothing — never leaks the Integer()-conversion error -
    {
        CHECK(ok("import(\"semver\").maxsatisfying([\"1.0.0\"], \"latest\")") == "None");
        CHECK(ok("import(\"semver\").minsatisfying([\"1.0.0\"], \"latest\")") == "None");
        CHECK(ok("String(import(\"semver\").satisfies(\"1.0.0\", \"latest\"))") == "False");
        CHECK(ok("String(import(\"semver\").validrange(\"latest\"))") == "False");
        CHECK(ok("String(import(\"semver\").satisfies(\"1.2.3\", \"^1.0.0\"))") == "True");        // valid range still works
    }

    // === I/O size guards: a huge read/seek must throw, never std::terminate or OOM ----------------
    {
        CHECK(has(err("var io = import(\"io\")\nvar b = io.BytesIO(\"ab\")\ndiscard b.seek(9000000000000000000)\nb.write(\"Z\")"),
                  "too large"));
        // file read with an absurd n is capped to the file's size, not pre-allocated
        CHECK(ok("var io = import(\"io\")\nvar f = io.open(\"/tmp/kaudit_reg.txt\", \"w\")\nf.write(\"hello\")\nf.close()\n"
                 "var g = io.open(\"/tmp/kaudit_reg.txt\", \"r\")\nvar s = g.read(9000000000000000000)\ng.close()\nlen(s)") == "5");
    }

    // === format: high precision must not be truncated to a fixed buffer; absurd precision is bounded
    {
        CHECK(ok("len(format(3.14159, \".200f\"))") == "202");                   // "3." + 200 digits, was capped at 63
        CHECK(ok("format(3.14159, \".2f\")") == "3.14");
        CHECK(has(err("discard format(1.0, \".3000000000f\")"), "too large"));   // precision bounded like width
    }

    // === round-2 re-audit crash regressions (each was a SIGSEGV/SIGABRT; must now throw cleanly) =====
    {
        // a non-class base was an unchecked static_cast<ClassValue&> -> SIGSEGV
        CHECK(has(err("var b = \"x\"\nclass C(b):\n    var z = 1\ndiscard C()"), "base class must be a class"));
        CHECK(has(err("class C(5):\n    var z = 1\ndiscard C()"), "base class must be a class"));
        // deeply-nested list to Tensor() recursed per level -> stack overflow; now bounded at 64 dims
        std::string deep = "var x = [1.0]\nvar i = 0\nwhile i < 5000:\n    x = [x]\n    i = i + 1\ndiscard import(\"tensor\").Tensor(x)\n";
        CHECK(has(err(deep), "too many dimensions"));
        // median over a 0-length axis read v[SIZE_MAX] -> SIGSEGV
        CHECK(has(err("import(\"tensor\").zeros([1, 0]).median(1)"), "empty axis"));
        // einsum on a non-tensor operand was an unchecked downcast (UB)
        CHECK(has(err("import(\"tensor\").einsum(\"i->\", 42)"), "Tensor"));
        // switch on a float matched by EXACT value (not a 6-digit string), agreeing with ==
        CHECK(ok("var f = Function(x):\n    switch x:\n        case 3.141593:\n            return 1\n        default:\n            return 0\nf(3.14159265358979)") == "0");
        CHECK(ok("var f = Function(x):\n    switch x:\n        case 0.0:\n            return 1\n        default:\n            return 0\nf(-0.0)") == "1");
        // randrange over a span wider than int64 throws instead of returning an out-of-range value
        CHECK(has(err("var r = import(\"random\").Random(1)\ndiscard r.randrange(-5000000000000000000, 5000000000000000000, 1)"), "too large"));
        // serialize.loads with a crafted overflowing/negative count throws (was signed-overflow UB)
        CHECK(has(err("import(\"serialize\").loads(\"KSER1 1 D 2000000000 0\")"), "corrupt"));
        CHECK(has(err("import(\"serialize\").loads(\"KSER1 1 L -3 0\")"), "corrupt"));
    }

    return RUN_TESTS();
}
