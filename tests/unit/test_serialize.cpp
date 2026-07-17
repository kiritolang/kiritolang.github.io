#include <filesystem>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

// round-trip a value through dumps/loads and stringify the result
static std::string roundTrip(KiritoVM& vm, const std::string& expr) {
    return evalStr(vm, "var s = import(\"serialize\")\ns.loads(s.dumps(" + expr + "))");
}

int main() {
    KiritoVM vm;

    // scalars
    CHECK(roundTrip(vm, "None") == "None");
    CHECK(roundTrip(vm, "True") == "True");
    CHECK(roundTrip(vm, "42") == "42");
    CHECK(roundTrip(vm, "-17") == "-17");
    CHECK(roundTrip(vm, "3.14159") == "3.14159");
    CHECK(roundTrip(vm, "\"héllo \\n world\"") == "héllo \n world");

    // containers
    CHECK(roundTrip(vm, "[1, 2, 3]") == "[1, 2, 3]");
    CHECK(roundTrip(vm, "[[1, 2], [3, [4, 5]]]") == "[[1, 2], [3, [4, 5]]]");
    CHECK(evalStr(vm, "var s = import(\"serialize\")\nlen(s.loads(s.dumps({1, 2, 3})))") == "3");
    CHECK(roundTrip(vm, "{\"a\": [1, 2], \"b\": \"x\"}") != "");  // (key order varies; checked below)
    CHECK(evalStr(vm, "var s = import(\"serialize\")\ns.loads(s.dumps({\"a\": [1, 2], \"b\": 3}))[\"a\"][1]") == "2");

    // structural equality after round-trip
    CHECK(evalStr(vm, R"(
var s = import("serialize")
var x = [1, [2, 3], {"k": 4}]
s.loads(s.dumps(x)) == x
)") == "True");

    // SHARED references are preserved: A appears twice -> one object after load
    CHECK(evalStr(vm, R"(
var s = import("serialize")
var A = [1]
var x = [A, A, [9]]
var y = s.loads(s.dumps(x))
y[0].append(2)
y[1]
)") == "[1, 2]");  // mutating y[0] is visible through y[1] (same object)
    CHECK(evalStr(vm, R"(
var s = import("serialize")
var A = [1]
var y = s.loads(s.dumps([A, A]))
y[0] == y[1]
)") == "True");

    // CYCLES round-trip and remain self-referential
    CHECK(evalStr(vm, R"(
var s = import("serialize")
var c = []
c.append(c)
var d = s.loads(s.dumps(c))
d[0] == d
)") == "True");

    // save / load to a file
    CHECK(evalStr(vm, R"(
var s = import("serialize")
var p = import("path").gettempdir() + "/kirito_serialize_test.dat"
var data = {"name": "Kirito", "scores": [10, 20, 30]}
s.save(data, p)
var loaded = s.load(p)
String(loaded["name"]) + ":" + String(loaded["scores"][2])
)") == "Kirito:30");
    std::filesystem::remove(std::filesystem::temp_directory_path() / "kirito_serialize_test.dat");

    // A native/built-in function is not serializable (a Kirito Function now IS — see below).
    CHECK_THROWS(vm.runSource("var s = import(\"serialize\")\ns.dumps(len)\n"));

    // Kirito functions/classes serialize by default through the text format too: a closure captures its
    // free variable by value, and an instance round-trips carrying its class (no import needed).
    CHECK(evalStr(vm, R"(
var s = import("serialize")
var k = 7
var mul = Function(x): return x * k
s.loads(s.dumps(mul))(6)
)") == "42");
    CHECK(evalStr(vm, R"(
var s = import("serialize")
class Box:
 var _init_ = Function(self, v): self.v = v
 var get = Function(self): return self.v
s.loads(s.dumps(Box(99))).get()
)") == "99");

    // corrupt / hostile text blobs throw cleanly (never crash) — exercises the shared rebuild's
    // bounds checks and the text decoder's error paths.
    CHECK_THROWS(vm.runSource("import(\"serialize\").loads(\"not a blob\")"));
    CHECK_THROWS(vm.runSource("import(\"serialize\").loads(\"\")"));
    CHECK_THROWS(vm.runSource("import(\"serialize\").loads(\"KSER1\")"));        // header, no count
    CHECK_THROWS(vm.runSource("import(\"serialize\").loads(\"KSER1 1 L 1 5 0\")"));  // child id out of range
    CHECK_THROWS(vm.runSource("import(\"serialize\").loads(\"KSER1 1 N 9\")"));  // root id out of range
    CHECK_THROWS(vm.runSource("import(\"serialize\").load(\"/nonexistent/path/xyz.dat\")"));

    // the shared core gives both modules identical graph semantics: a value's serialize and dump
    // round-trips agree structurally (shared refs + cycles), differing only in format/type.
    CHECK(evalStr(vm, R"(
var s = import("serialize")
var d = import("dump")
var A = [1, 2]
var g = {"x": A, "y": A}
g["self"] = g
var fromText = s.loads(s.dumps(g))
var fromBin = d.loads(d.dumps(g))
var ok = (fromText["x"] == fromText["y"]) and (fromText["self"] == fromText)
ok = ok and (fromBin["x"] == fromBin["y"]) and (fromBin["self"] == fromBin)
ok = ok and (fromText["x"] == fromBin["x"])
ok
)") == "True");

    return RUN_TESTS();
}
