#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

// All checks below read the CURRENT clock or measure a real sleep, so their output is
// inherently non-reproducible (see tests/scripts/unit_time.ki for the fixed-epoch/fixed-input
// counterparts, which cover field extraction, iso/format, round-trip, and bad-argument-type
// checks — everything that doesn't need a live clock or an actual sleep).
int main() {
    KiritoVM vm;

    // clocks return sane, monotonic-ish values
    CHECK(evalStr(vm, "import(\"time\").time() > 1000000000.0") == "True");
    CHECK(evalStr(vm, "import(\"time\").timens() > 1000000000000000000") == "True");
    CHECK(evalStr(vm, R"(
var t = import("time")
var a = t.monotonic()
var b = t.monotonic()
b >= a
)") == "True");
    CHECK(evalStr(vm, R"(
var t = import("time")
var a = t.perfcounterns()
var b = t.perfcounterns()
b >= a
)") == "True");

    // datetime() with no arg and now() both return a DateTime
    CHECK(evalStr(vm, "type(import(\"time\").datetime())") == "DateTime");
    CHECK(evalStr(vm, "type(import(\"time\").now())") == "DateTime");
    CHECK(evalStr(vm, "import(\"time\").now().year >= 2024") == "True");

    // sleep with a tiny duration returns None and doesn't hang
    CHECK(evalStr(vm, "import(\"time\").sleep(0.001)") == "None");
    CHECK(evalStr(vm, "import(\"time\").sleep(0)") == "None");

    return RUN_TESTS();
}
