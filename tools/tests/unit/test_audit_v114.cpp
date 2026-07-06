// Regression tests for the v1.14 audit (.audit/v1.14/). Each block pins one confirmed finding so it
// can never silently regress. Memory-safety items (A06-1 deep-_str_ native recursion) are also a
// crash gate — run under -fsanitize=address,undefined for the strongest signal.
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

int main() {
    // === A09-1 (MEDIUM): the format() builtin with an EMPTY spec was lossy for Floats — it routed
    // through applyFormatSpec's default ('g'/precision-6), so `format(2.0)` -> "2" and
    // `format(1234567.0)` -> "1.23457e+06", diverging from f-strings / "{}".format / String() (all
    // stringify). Empty spec now short-circuits to stringify, so a Float round-trips. ===
    CHECK(ok("format(2.0)") == "2.0");
    CHECK(ok("format(1234567.0)") == "1234567.0");
    CHECK(ok("format(0.1)") == "0.1");
    CHECK(ok("format(42)") == "42");           // Integer unchanged
    CHECK(ok("format(3.5)") == "3.5");
    // a NON-empty spec still applies the mini-format-spec (no behaviour change there)
    CHECK(ok("format(2.0, \".2f\")") == "2.00");
    CHECK(ok("format(255, \"x\")") == "ff");
    // consistency: format(x) with no spec now equals String(x) for a Float, as the doc-comment claims
    CHECK(ok("format(2.0) == String(2.0)") == "True");

    // === A06-1 (MEDIUM, native recursion / crash): InstanceValue::str's cycle guard only caught a
    // _str_ that returns SELF; a _str_ that returns a chain of DISTINCT instances recursed in native
    // C++ with no depth bound -> stack overflow (segfault). Now bounded by ctx.depth like the
    // container stringifier: it throws a clean, catchable error instead of crashing. ===
    CHECK(has(err(
        "class N:\n"
        "    var _init_ = Function(self, nxt): self.nxt = nxt\n"
        "    var _str_ = Function(self): return self.nxt\n"
        "var head = None\n"
        "var i = 0\n"
        "while i < 5000:\n"
        "    head = N(head)\n"
        "    i = i + 1\n"
        "String(head)"), "recurses too deeply"));
    // a shallow _str_-returns-instance chain still works (the guard doesn't over-reject)
    CHECK(ok("class W:\n"
             "    var _init_ = Function(self, v): self.v = v\n"
             "    var _str_ = Function(self): return self.v\n"
             "String(W(W(\"deep\")))") == "deep");

    // === A16-1 (MEDIUM, UB): time.sleep() had no finite/range guard, so sleep(inf)/sleep(nan)
    // overflowed the int64 duration_cast (UB, returned immediately) and sleep(1e18) hung. Both are
    // now rejected before sleep_for. (A tiny real sleep still works.) ===
    CHECK(has(err("import(\"time\").sleep(import(\"math\").nan)"), "finite"));
    CHECK(has(err("import(\"time\").sleep(import(\"math\").inf)"), "finite"));
    CHECK(has(err("import(\"time\").sleep(1e18)"), "too large"));
    CHECK(ok("import(\"time\").sleep(0.0)\n42") == "42");                        // <=0 is a no-op
    CHECK(ok("import(\"time\").sleep(-5.0)\n7") == "7");

    // === A14-2 (MEDIUM, signed-overflow UB): json.stringify's indent path computed (depth+1)*indent
    // in int, overflowing for a huge indent (and OOMing with a raw allocator message short of that).
    // A too-large indent is now a clean error; a sane indent still pretty-prints. ===
    CHECK(has(err("import(\"json\").stringify([1, 2], indent = 99999999)"), "indent too large"));
    CHECK(has(err("import(\"json\").stringify([1], indent = 100000000000)"), "indent too large"));
    CHECK(ok("import(\"json\").stringify([1, 2], indent = 2)") == "[\n  1,\n  2\n]");
    CHECK(ok("import(\"json\").stringify([1, 2])") == "[1, 2]");                 // compact default

    // === A15-1 (MEDIUM): Socket.listen() read sock.fd directly instead of fdOrThrow, so
    // listen-after-close leaked a raw "Bad file descriptor" instead of the contractual
    // "listen: socket is closed" every other Socket method gives. ===
    CHECK(has(err("var s = import(\"net\").tcpsocket()\ns.close()\ns.listen(16)"),
              "listen: socket is closed"));

    return RUN_TESTS();
}
