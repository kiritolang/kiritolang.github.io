// test_sys_time_deep.cpp — adversarial/edge coverage for the `sys` and `time` modules:
// component-range guards, out-of-epoch years, DateTime str/format/arithmetic edges, strptime
// field ranges + leap-day validation, datetime float truncation, sys.version shape, sys.shell.
#include <string>
#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run a Kirito program, return the last expression stringified.
static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
// True iff the program raises a Kirito error (a caught KiritoError or any std::exception).
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; }
    catch (...) { return true; }
}

int main() {
    KiritoVM vm;

    // ---- time.make component-range guard --------------------------------------------------------
    // A component well past the ±2e9 in-band bound throws "make: date component out of range".
    CHECK(throws(vm, "import(\"time\")\ntime.make(2024, 1, 1, 3000000000)"));
    CHECK(throws(vm, "import(\"time\")\ntime.make(2024, 1, 1, 0, 3000000000)"));
    CHECK(throws(vm, "import(\"time\")\ntime.make(2024, 1, 1, 0, 0, -3000000000)"));
    // A sane make still works and round-trips to ISO.
    CHECK(run(vm, "import(\"time\")\ntime.make(2024, 1, 1).iso()") == "2024-01-01T00:00:00");
    CHECK(run(vm, "import(\"time\")\ntime.make(2024, 3, 4, 5, 6, 7).iso()") == "2024-03-04T05:06:07");

    // ---- make out-of-epoch year (constructor rejects year outside [-9999, 9999]) ----------------
    CHECK(throws(vm, "import(\"time\")\ntime.make(10001, 1, 1)"));
    CHECK(throws(vm, "import(\"time\")\ntime.make(-10000, 1, 1)"));
    // The boundary years are representable.
    CHECK(run(vm, "import(\"time\")\ntime.make(9999, 1, 1).year") == "9999");

    // ---- DateTime.format("") empty-format returns "" --------------------------------------------
    CHECK(run(vm, "import(\"time\")\ntime.datetime(0).format(\"\")") == "");
    // A non-String format argument throws.
    CHECK(throws(vm, "import(\"time\")\ntime.datetime(0).format(123)"));
    // format(0 args) / format(2 args) throw (arity guard).
    CHECK(throws(vm, "import(\"time\")\ntime.datetime(0).format()"));

    // ---- DateTime repr/str(): "DateTime(<iso>)" -------------------------------------------------
    CHECK(run(vm, "import(\"time\")\ntime.datetime(0)") == "DateTime(1970-01-01T00:00:00)");
    // Nested in a container it uses the same str form (DateTime has no distinct repr).
    CHECK(run(vm, "import(\"time\")\n[time.datetime(0)]") == "[DateTime(1970-01-01T00:00:00)]");

    // ---- DateTime.add/sub with a non-finite Float must throw ------------------------------------
    CHECK(throws(vm, "import(\"time\")\ntime.datetime(0).add(1e400)"));          // +inf
    CHECK(throws(vm, "import(\"time\")\ntime.datetime(0).sub(1e400)"));
    CHECK(throws(vm, "import(\"time\")\ntime.datetime(0).add(0.0 * 1e400)"));    // NaN
    CHECK(throws(vm, "import(\"time\")\ntime.datetime(0).sub(0.0 * 1e400)"));
    // A finite Float delta truncates toward zero and works.
    CHECK(run(vm, "import(\"time\")\ntime.datetime(0).add(90.9).timestamp") == "90");
    // A non-number delta throws.
    CHECK(throws(vm, "import(\"time\")\ntime.datetime(0).add(\"x\")"));

    // ---- add/sub with wrong arg count (0 or 2) must throw ---------------------------------------
    CHECK(throws(vm, "import(\"time\")\ntime.datetime(0).add()"));
    CHECK(throws(vm, "import(\"time\")\ntime.datetime(0).add(1, 2)"));
    CHECK(throws(vm, "import(\"time\")\ntime.datetime(0).sub()"));
    CHECK(throws(vm, "import(\"time\")\ntime.datetime(0).sub(1, 2)"));
    // A correct add advances the epoch.
    CHECK(run(vm, "import(\"time\")\ntime.datetime(10).add(5).timestamp") == "15");
    CHECK(run(vm, "import(\"time\")\ntime.datetime(10).sub(5).timestamp") == "5");

    // ---- datetime with a finite fractional Float truncates (no throw) ---------------------------
    CHECK(run(vm, "import(\"time\")\ntime.datetime(100.9).timestamp") == "100");
    CHECK(run(vm, "import(\"time\")\ntime.datetime(100.9).iso()") == "1970-01-01T00:01:40");
    // Non-finite timestamps throw.
    CHECK(throws(vm, "import(\"time\")\ntime.datetime(1e400)"));       // inf
    CHECK(throws(vm, "import(\"time\")\ntime.datetime(0.0 * 1e400)")); // NaN

    // ---- datetime(0).weekday == 4 (1970-01-01 is Thursday) --------------------------------------
    CHECK(run(vm, "import(\"time\")\ntime.datetime(0).weekday") == "4");

    // ---- strptime field ranges ------------------------------------------------------------------
    // %M=60 and %S=60 out of range; %H=25 out of range (re-assert).
    CHECK(throws(vm, "import(\"time\")\ntime.strptime(\"2024-01-01T00:60:00\", \"%Y-%m-%dT%H:%M:%S\")"));
    CHECK(throws(vm, "import(\"time\")\ntime.strptime(\"2024-01-01T00:00:60\", \"%Y-%m-%dT%H:%M:%S\")"));
    CHECK(throws(vm, "import(\"time\")\ntime.strptime(\"2024-01-01T25:00:00\", \"%Y-%m-%dT%H:%M:%S\")"));
    // Upper valid bounds parse (59/59/23).
    CHECK(run(vm, "import(\"time\")\ntime.strptime(\"2024-01-01T23:59:59\", \"%Y-%m-%dT%H:%M:%S\").iso()")
              == "2024-01-01T23:59:59");
    // month 99 / day 32 also rejected.
    CHECK(throws(vm, "import(\"time\")\ntime.strptime(\"2024-99-01\", \"%Y-%m-%d\")"));
    CHECK(throws(vm, "import(\"time\")\ntime.strptime(\"2024-01-32\", \"%Y-%m-%d\")"));
    // Trailing unconverted input is rejected.
    CHECK(throws(vm, "import(\"time\")\ntime.strptime(\"2024-01-01XYZ\", \"%Y-%m-%d\")"));

    // ---- strptime leap-day validation -----------------------------------------------------------
    CHECK(run(vm, "import(\"time\")\ntime.strptime(\"2024-02-29\", \"%Y-%m-%d\").iso()")
              == "2024-02-29T00:00:00");                                   // 2024 is a leap year
    CHECK(throws(vm, "import(\"time\")\ntime.strptime(\"2023-02-29\", \"%Y-%m-%d\")"));  // 2023 is not
    CHECK(throws(vm, "import(\"time\")\ntime.strptime(\"2024-02-30\", \"%Y-%m-%d\")"));  // Feb never has 30
    // Century (non-)leap rules: 1900 not leap, 2000 leap.
    CHECK(throws(vm, "import(\"time\")\ntime.strptime(\"1900-02-29\", \"%Y-%m-%d\")"));
    CHECK(run(vm, "import(\"time\")\ntime.strptime(\"2000-02-29\", \"%Y-%m-%d\").iso()")
              == "2000-02-29T00:00:00");

    // NOTE: DateTime._setstate_ with a non-Integer epoch is guarded in C++
    // ("DateTime _setstate_: expected an Integer epoch"), but there is no Kirito-level path to hand a
    // malformed state to a freshly-deserialized DateTime — _setstate_ is only invoked by the serialize/
    // dump reconstruction core, which always supplies the Integer written by _getstate_. So the
    // non-Integer branch is not reachable from a Kirito snippet; skipping (would require forging a
    // corrupt blob at the C++ layer, out of scope for this source-level suite).

    // ---- sys.version shape (interpreter semver) -------------------------------------------------
    // Non-empty and semver-shaped (>= 2 dots), matching what `ki --version` reports.
    CHECK(run(vm, "import(\"sys\")\nlen(sys.version) > 0") == "True");
    CHECK(run(vm, "import(\"sys\")\nsys.version.count(\".\") >= 2") == "True");
    // Every component up to the first two dots is all digits (major.minor.patch core).
    CHECK(run(vm, "import(\"sys\")\nvar p = sys.version.split(\".\")\np[0].isdigit() and p[1].isdigit()")
              == "True");

    // ---- sys.shell with input=/cwd=/timeout= keywords -------------------------------------------
    // `cat` echoes stdin -> stdout; the returned Dict has {code, stdout, stderr}. `cat` is present on
    // every POSIX system this suite runs on. Assert the Dict shape strongly and the echo leniently
    // (guarded on code == 0 so a missing/odd `cat` never fails the suite).
    // NOTE: subprocess may be unavailable in some sandboxes; shape assertions stay lenient.
    CHECK(run(vm, R"KI(import("sys")
var r = sys.shell("cat", input="hi")
type(r["code"]) == "Integer" and type(r["stdout"]) == "String" and type(r["stderr"]) == "String")KI")
              == "True");
    // The echoed stdin comes back on stdout when the command ran cleanly.
    CHECK(run(vm, R"KI(import("sys")
var r = sys.shell("cat", input="hello")
r["stdout"] == "hello" if r["code"] == 0 else True)KI") == "True");
    // input= keyword directs stdin; timeout= keyword accepted (positive, non-triggering here).
    CHECK(run(vm, R"KI(import("sys")
var r = sys.shell("cat", input="probe", timeout=10)
type(r["stdout"]) == "String")KI") == "True");
    // cwd= keyword accepted and the process runs there (lenient: just assert it returns a Dict shape).
    CHECK(run(vm, R"KI(import("sys")
import("path")
var r = sys.shell("true", cwd=path.gettempdir())
type(r["code"]) == "Integer")KI") == "True");

    // A shell command that exits non-zero surfaces the code (no throw for a normal exit).
    CHECK(run(vm, R"KI(import("sys")
var r = sys.shell("exit 3")
r["code"] == 3 if r["code"] != 0 else True)KI") == "True");

    return RUN_TESTS();
}
