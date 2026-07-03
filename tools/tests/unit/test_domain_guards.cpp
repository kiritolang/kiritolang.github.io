// Regression tests for the "silent bug on bad input" pass: math/complex/tensor domain errors,
// malformed Integer/Float strings, pow negative modulus, statistics.quantiles n, tabular Series
// length-mismatch + ragged CSV, and time.strptime range/trailing checks must all RAISE a clear,
// catchable error instead of returning NaN/inf/garbage. Runs through a bare embedded KiritoVM so the
// embedding path is covered too. Run under -fsanitize=address,undefined to confirm no UB leaks.
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
static bool thrown(const std::string& src) { return !err(src).empty(); }
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // === math: domain errors throw; NaN passes through; overflow-to-inf is NOT a domain error ===
    CHECK(has(err("discard import(\"math\").sqrt(-1)"), "math domain error"));
    CHECK(has(err("discard import(\"math\").log(0)"), "math domain error"));
    CHECK(thrown("discard import(\"math\").log(-1)"));
    CHECK(thrown("discard import(\"math\").asin(2)"));
    CHECK(thrown("discard import(\"math\").acos(2)"));
    CHECK(thrown("discard import(\"math\").acosh(0)"));
    CHECK(thrown("discard import(\"math\").atanh(1)"));
    CHECK(thrown("discard import(\"math\").log2(0)"));
    CHECK(thrown("discard import(\"math\").log10(0)"));
    CHECK(thrown("discard import(\"math\").log1p(-1)"));
    CHECK(thrown("discard import(\"math\").gamma(0)"));
    CHECK(thrown("discard import(\"math\").gamma(-2)"));
    CHECK(thrown("discard import(\"math\").pow(-2, 0.5)"));   // negative base, fractional exponent
    CHECK(thrown("discard import(\"math\").pow(0, -1)"));     // zero to a negative power
    CHECK(thrown("discard import(\"math\").fmod(1, 0)"));     // divisor zero
    CHECK(thrown("discard import(\"math\").log(8, 1)"));      // base 1
    // NaN passes through, overflow→inf does not throw, valid input still computes
    CHECK(ok("import(\"math\").isnan(import(\"math\").sqrt(import(\"math\").nan))") == "True");
    CHECK(ok("import(\"math\").isinf(import(\"math\").exp(1000))") == "True");
    CHECK(ok("import(\"math\").sqrt(16)") == "4.0");
    // prod overflow throws like its siblings; a Float in the mix never overflows
    CHECK(thrown("discard import(\"math\").prod([1000000000000, 1000000000000])"));
    CHECK(ok("import(\"math\").prod([2.0, 3.0])") == "6.0");

    // === complex: domain errors (log/log10 of 0, pow zero-to-negative, atanh +/-1 throw) ===
    CHECK(has(err("discard import(\"complex\").log(import(\"complex\").zero)"), "math domain error"));
    CHECK(thrown("discard import(\"complex\").log10(import(\"complex\").zero)"));
    CHECK(has(err("discard import(\"complex\").pow(import(\"complex\").zero, import(\"complex\").of(0-1,0))"),
              "zero to a negative"));
    CHECK(thrown("discard import(\"complex\").zero ** import(\"complex\").of(0-1, 0)"));
    CHECK(thrown("discard import(\"complex\").atanh(import(\"complex\").one)"));
    // valid in the complex plane — must NOT throw
    CHECK(!thrown("discard import(\"complex\").sqrt(import(\"complex\").of(0-1, 0))"));
    CHECK(!thrown("discard import(\"complex\").log(import(\"complex\").of(0-1, 0))"));
    CHECK(!thrown("discard import(\"complex\").acosh(import(\"complex\").zero)"));

    // === tensor: elementwise math domain + reciprocal(0) + clip(lo>hi) + norm(ord=0) ===
    CHECK(has(err("discard import(\"tensor\").Tensor([0-1.0]).sqrt()"), "tensor sqrt: math domain error"));
    CHECK(thrown("discard import(\"tensor\").Tensor([0.0]).log()"));
    CHECK(thrown("discard import(\"tensor\").Tensor([2.0]).asin()"));
    CHECK(thrown("discard import(\"tensor\").Tensor([2.0]).atanh()"));
    CHECK(thrown("discard import(\"tensor\").Tensor([0.5]).acosh()"));
    CHECK(has(err("discard import(\"tensor\").Tensor([0.0]).reciprocal()"), "division by zero"));
    CHECK(thrown("discard import(\"tensor\").Tensor([0-2.0]).pow(0.5)"));
    CHECK(thrown("discard import(\"tensor\").Tensor([1.0]).clip(5.0, 1.0)"));
    CHECK(ok("import(\"tensor\").norm(import(\"tensor\").Tensor([3.0, 0.0, 4.0]), 0.0)") == "2.0");
    // valid: sqrt(0)=0, integer exponent on a negative base
    CHECK(ok("import(\"tensor\").Tensor([0.0, 4.0]).sqrt().tolist()") == "[0.0, 2.0]");
    CHECK(ok("import(\"tensor\").Tensor([0-8.0]).pow(3.0).tolist()") == "[-512.0]");

    // === builtins: malformed Integer/Float strings + pow negative modulus ===
    CHECK(thrown("discard Integer(\"--5\")"));
    CHECK(thrown("discard Integer(\"+ 5\")"));
    CHECK(thrown("discard Integer(\"0x-5\")"));
    CHECK(thrown("discard Integer(\"0b -1\")"));
    CHECK(ok("Integer(\"  -5 \")") == "-5");
    CHECK(ok("Integer(\"-0xFF\")") == "-255");
    CHECK(thrown("discard Float(\"0x1p4\")"));               // C99 hex float rejected
    CHECK(ok("Float(\"1.5e3\")") == "1500.0");
    CHECK(has(err("discard pow(7, 2, 0-5)"), "modulus must be positive"));
    CHECK(ok("pow(2, 10, 1000)") == "24");

    // === statistics.quantiles: n must be >= 1 ===
    CHECK(thrown("discard import(\"statistics\").quantiles([1,2,3], 0)"));
    CHECK(thrown("discard import(\"statistics\").quantiles([1,2,3], 0-2)"));
    CHECK(ok("len(import(\"statistics\").quantiles([1,2,3,4], 4))") == "3");

    // === tabular: Series length mismatch (both directions) + ragged CSV ===
    CHECK(has(err("var p = import(\"tabular\")\ndiscard p.Series([1,2]) + p.Series([1,2,3])"), "length mismatch"));
    CHECK(thrown("var p = import(\"tabular\")\ndiscard p.Series([1,2,3]) + p.Series([1,2])"));  // symmetric
    CHECK(has(err("discard import(\"tabular\").readcsv(\"a,b\\n1,2,3,4\")"), "fields, expected"));
    CHECK(ok("var p = import(\"tabular\")\n(p.Series([1,2,3]) + p.Series([10,20,30])).tolist()") == "[11, 22, 33]");

    // === time.strptime: out-of-range fields + trailing data throw; make() rollover unchanged ===
    CHECK(thrown("discard import(\"time\").strptime(\"2024-99-99\", \"%Y-%m-%d\")"));
    CHECK(thrown("discard import(\"time\").strptime(\"2024-01-01XYZ\", \"%Y-%m-%d\")"));
    CHECK(ok("import(\"time\").strptime(\"2024-06-15\", \"%Y-%m-%d\").iso()") == "2024-06-15T00:00:00");
    CHECK(ok("import(\"time\").make(2024, 13, 1).year") == "2025");  // deliberate C-mktime rollover

    return RUN_TESTS();
}
