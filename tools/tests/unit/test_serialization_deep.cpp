// test_serialization_deep.cpp — adversarial/edge coverage for json/serialize/dump, filling audited
// gaps: the serde flatten deep-nesting guard, json indent>0 rejection branches, number
// overflow-to-infinity, subclass + None-state round-trips, \b\f\r parse+emit escapes, Float dict
// keys, and uppercase-E exponents.
#include <string>
#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; }
    catch (...) { return true; }
}

int main() {
    KiritoVM vm;

    // ---- serde flatten deep-nesting guard: 20000-deep throws for BOTH text and binary codecs ----
    CHECK(throws(vm, R"KI(var serialize = import("serialize")
var x = 0
var i = 0
while i < 20000:
    x = [x]
    i = i + 1
serialize.dumps(x))KI"));
    CHECK(throws(vm, R"KI(var dump = import("dump")
var x = 0
var i = 0
while i < 20000:
    x = [x]
    i = i + 1
dump.dumps(x))KI"));
    // a modestly nested structure round-trips fine
    CHECK(run(vm, R"KI(var serialize = import("serialize")
var x = 0
var i = 0
while i < 50:
    x = [x]
    i = i + 1
serialize.loads(serialize.dumps(x))[0][0][0] != None)KI") == "True");

    // ---- json.dumps rejection branches under indent>0 (cyclic / Set / Function) ----
    CHECK(throws(vm, R"KI(var json = import("json")
var a = []
a.append(a)
json.dumps(a, indent=2))KI"));
    CHECK(throws(vm, "var json = import(\"json\")\njson.dumps({1, 2}, indent=2)"));
    CHECK(throws(vm, "var json = import(\"json\")\njson.dumps(Function(x): return x, indent=2)"));

    // ---- json number overflow parses to ±Infinity ----
    CHECK(run(vm, "var json = import(\"json\")\nvar math = import(\"math\")\njson.parse(\"1e400\") == math.inf") == "True");
    CHECK(run(vm, "var json = import(\"json\")\nvar math = import(\"math\")\njson.parse(\"-1e400\") == -math.inf") == "True");
    // uppercase-E exponent scans correctly
    CHECK(run(vm, "var json = import(\"json\")\njson.parse(\"1E5\")") == "100000.0");

    // ---- subclass instance round-trip preserves inherited + own attributes ----
    CHECK(run(vm, R"KI(var serialize = import("serialize")
class Base:
    var _init_ = Function(self, x):
        self.x = x
class Sub(Base):
    var _init_ = Function(self, x, y):
        self.x = x
        self.y = y
var r = serialize.loads(serialize.dumps(Sub(1, 2)))
r.x + r.y)KI") == "3");

    // ---- _getstate_ returning None round-trips (falsy state link) ----
    CHECK(run(vm, R"KI(var serialize = import("serialize")
class Marker:
    var _getstate_ = Function(self):
        return None
    var _setstate_ = Function(self, st):
        self.restored = True
serialize.loads(serialize.dumps(Marker())).restored)KI") == "True");

    // ---- json parse of \b \f \r control-char escapes ----
    CHECK(run(vm, R"KI(var json = import("json")
ord(json.parse(r'"\b"')))KI") == "8");
    CHECK(run(vm, R"KI(var json = import("json")
ord(json.parse(r'"\f"')))KI") == "12");
    CHECK(run(vm, R"KI(var json = import("json")
ord(json.parse(r'"\r"')))KI") == "13");

    // ---- json output escaping of \b \f \r (the dumped text contains the backslash escape) ----
    CHECK(run(vm, R"KI(var json = import("json")
r"\b" in json.dumps("\x08"))KI") == "True");
    CHECK(run(vm, R"KI(var json = import("json")
r"\f" in json.dumps("\x0c"))KI") == "True");
    CHECK(run(vm, R"KI(var json = import("json")
r"\r" in json.dumps("\r"))KI") == "True");

    // ---- json Float dict key stringifies to a quoted number ----
    CHECK(run(vm, R"KI(var json = import("json")
"2.5" in json.dumps({2.5: "v"}))KI") == "True");

    // NOTE: the serialize-text 'S' explicit-length overrun guard is reachable only via a hand-crafted
    // KSER1 blob; its binary `dump` equivalent is already covered directly, so it is not re-done here.

    return RUN_TESTS();
}
