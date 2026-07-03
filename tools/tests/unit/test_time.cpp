#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

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

    // datetime field extraction from a known UTC timestamp (2021-01-01 00:00:00Z = 1609459200)
    CHECK(evalStr(vm, "import(\"time\").datetime(1609459200).year") == "2021");
    CHECK(evalStr(vm, "import(\"time\").datetime(1609459200).month") == "1");
    CHECK(evalStr(vm, "import(\"time\").datetime(1609459200).day") == "1");
    CHECK(evalStr(vm, "import(\"time\").datetime(1609459200).hour") == "0");
    CHECK(evalStr(vm, "import(\"time\").datetime(1609459200).iso()") == "2021-01-01T00:00:00");
    CHECK(evalStr(vm, "import(\"time\").datetime(1609459200).timestamp") == "1609459200");

    // a precise mid-day timestamp: 2021-06-15 12:30:45 UTC = 1623760245
    CHECK(evalStr(vm, "import(\"time\").datetime(1623760245).iso()") == "2021-06-15T12:30:45");
    CHECK(evalStr(vm, "import(\"time\").datetime(1623760245).hour") == "12");
    CHECK(evalStr(vm, "import(\"time\").datetime(1623760245).minute") == "30");
    CHECK(evalStr(vm, "import(\"time\").datetime(1623760245).second") == "45");

    // weekday: 2021-01-01 was a Friday (tm_wday=5)
    CHECK(evalStr(vm, "import(\"time\").datetime(1609459200).weekday") == "5");

    // strftime formatting
    CHECK(evalStr(vm, "import(\"time\").datetime(1609459200).format(\"%Y/%m/%d\")") == "2021/01/01");

    // the epoch itself
    CHECK(evalStr(vm, "import(\"time\").datetime(0).iso()") == "1970-01-01T00:00:00");

    // datetime() with no arg and now() both return a DateTime
    CHECK(evalStr(vm, "type(import(\"time\").datetime())") == "DateTime");
    CHECK(evalStr(vm, "type(import(\"time\").now())") == "DateTime");
    CHECK(evalStr(vm, "import(\"time\").now().year >= 2024") == "True");

    // round-trip: datetime(ts).timestamp == ts
    CHECK(evalStr(vm, R"(
var t = import("time")
t.datetime(1700000000).timestamp == 1700000000
)") == "True");

    // sleep with a tiny duration returns None and doesn't hang
    CHECK(evalStr(vm, "import(\"time\").sleep(0.001)") == "None");
    CHECK(evalStr(vm, "import(\"time\").sleep(0)") == "None");

    // hostile: bad argument types throw
    CHECK_THROWS(vm.runSource("import(\"time\").datetime(\"notanumber\")"));
    CHECK_THROWS(vm.runSource("import(\"time\").sleep(\"x\")"));
    CHECK_THROWS(vm.runSource("import(\"time\").datetime(0).format(42)"));

    return RUN_TESTS();
}
