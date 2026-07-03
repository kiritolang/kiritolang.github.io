// Native stdlib VALUE types (Matrix/Vector/Complex/ComplexMatrix/DateTime/Random/Tensor) round-trip
// through the C++ serialize (text) and dump (binary) codecs via their _getstate_/_setstate_ protocol,
// preserving value; resource-like natives still throw. Also checks DateTime value-equality + hashing.
// End-to-end pure-Kirito coverage lives in tests/scripts/spec_serialize_native.ki.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static bool valEq(KiritoVM& vm, Handle a, Handle b) {
    return vm.arena().deref(a).equals(vm.arena(), vm.arena().deref(b));
}

int main() {
    KiritoVM vm;
    vm.installStandardLibrary();

    // Each value built by a Kirito expression round-trips, unchanged, through BOTH graph codecs.
    auto rtBoth = [&](const char* expr) {
        RootScope rs(vm);
        Handle v = rs.add(vm.runSource(expr));
        Handle t = rs.add(serial::loads(vm, serial::dumps(vm, v)));   // text
        CHECK(valEq(vm, v, t));
        Handle d = rs.add(dumpfmt::read(vm, dumpfmt::write(vm, v)));  // binary
        CHECK(valEq(vm, v, d));
    };
    rtBoth("import(\"matrix\").Matrix([[1.0, 2.0], [3.0, 4.0]])");
    rtBoth("import(\"matrix\").vector([1.0, 2.0, 3.0])");
    rtBoth("import(\"matrix\").identity(5)");
    rtBoth("import(\"complex\").of(1.5, -2.5)");
    rtBoth("import(\"complex\").Matrix([[import(\"complex\").of(1.0, 2.0)]])");
    rtBoth("import(\"time\").make(2021, 6, 15, 8, 30, 0)");
    rtBoth("import(\"tensor\").Tensor([[1.0, 2.0], [3.0, 4.0]])");

    // DateTime value equality (same instant -> equal, despite being distinct objects).
    CHECK(valEq(vm, vm.runSource("import(\"time\").make(2000, 1, 1)"),
                    vm.runSource("import(\"time\").make(2000, 1, 1)")));
    CHECK(!valEq(vm, vm.runSource("import(\"time\").make(2000, 1, 1)"),
                     vm.runSource("import(\"time\").make(2000, 1, 2)")));
    // DateTime is hashable -> usable as a Dict key.
    CHECK(vm.stringify(vm.runSource(
        "var t = import(\"time\")\nvar d = {t.make(2000, 1, 1): 7}\nd[t.make(2000, 1, 1)]")) == "7");

    // Random restores its exact stream after a dump/load checkpoint.
    CHECK(vm.stringify(vm.runSource(
        "var r = import(\"random\").Random(5)\n"
        "discard r.random()\n"
        "var blob = import(\"dump\").dumps(r)\n"
        "var a = r.randint(0, 1000000)\n"
        "var r2 = import(\"dump\").loads(blob)\n"
        "var b = r2.randint(0, 1000000)\n"
        "a == b")) == "True");

    // Resource-like natives are not serializable: a clean, catchable error (never a crash).
    CHECK_THROWS(serial::dumps(vm, vm.runSource("import(\"net\").Socket()")));
    CHECK_THROWS(dumpfmt::write(vm, vm.runSource("import(\"io\").BytesIO()")));
    CHECK_THROWS(serial::dumps(vm, vm.runSource("import(\"regex\").compile(\"a\")")));

    return RUN_TESTS();
}
