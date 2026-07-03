#include <filesystem>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

// round-trip an expression through dumps/loads and stringify the result
static std::string roundTrip(KiritoVM& vm, const std::string& expr) {
    return evalStr(vm, "var d = import(\"dump\")\nd.loads(d.dumps(" + expr + "))");
}

int main() {
    KiritoVM vm;

    // scalars
    CHECK(roundTrip(vm, "None") == "None");
    CHECK(roundTrip(vm, "True") == "True");
    CHECK(roundTrip(vm, "False") == "False");
    CHECK(roundTrip(vm, "42") == "42");
    CHECK(roundTrip(vm, "-1000000") == "-1000000");
    CHECK(roundTrip(vm, "3.14159265358979") == "3.14159265358979");
    CHECK(roundTrip(vm, "\"hello world\"") == "hello world");
    // binary-safe strings (embedded NUL and high bytes)
    CHECK(evalStr(vm, R"(
var d = import("dump")
var s = "a\x00b"
len(d.loads(d.dumps(s)))
)") == "3");

    // containers
    CHECK(roundTrip(vm, "[1, 2, 3]") == "[1, 2, 3]");
    CHECK(roundTrip(vm, "[[1, [2, 3]], 4]") == "[[1, [2, 3]], 4]");
    CHECK(evalStr(vm, "var d = import(\"dump\")\nd.loads(d.dumps({1, 2, 3})).contains(2)") == "True");
    CHECK(evalStr(vm, "var d = import(\"dump\")\nd.loads(d.dumps({\"a\": [1, 2], \"b\": 3}))[\"a\"][1]") == "2");

    // structural equality survives the round-trip
    CHECK(evalStr(vm, R"(
var d = import("dump")
var x = [1, [2, 3], {"k": 4}]
d.loads(d.dumps(x)) == x
)") == "True");

    // dumps returns Bytes (the binary blob); loads accepts the Bytes (or a String of the same bytes)
    CHECK(evalStr(vm, "type(import(\"dump\").dumps([1, 2, 3]))") == "Bytes");
    CHECK(evalStr(vm, "len(import(\"dump\").dumps([1, 2, 3])) > 0") == "True");
    CHECK(evalStr(vm, R"(
var d = import("dump")
var blob = d.dumps([10, 20, 30])
d.loads(blob)
)") == "[10, 20, 30]");
    // the Bytes blob round-trips through latin-1 String and back (loads accepts either)
    CHECK(evalStr(vm, R"(
var d = import("dump")
var blob = d.dumps([10, 20, 30])
d.loads(blob.decode("latin-1").encode("latin-1"))
)") == "[10, 20, 30]");

    // SHARED references preserved: A appears twice -> one object after load
    CHECK(evalStr(vm, R"(
var d = import("dump")
var A = [1]
var y = d.loads(d.dumps([A, A]))
y[0].append(2)
y[1]
)") == "[1, 2]");
    CHECK(evalStr(vm, R"(
var d = import("dump")
var A = [1]
var y = d.loads(d.dumps([A, A]))
y[0] == y[1]
)") == "True");

    // CYCLES round-trip
    CHECK(evalStr(vm, R"(
var d = import("dump")
var c = []
c.append(c)
var r = d.loads(d.dumps(c))
r[0] == r
)") == "True");

    // save / load to a binary file
    CHECK(evalStr(vm, R"(
var d = import("dump")
var f = import("path").gettempdir() + "/kirito_dump_test.bin"
var data = {"name": "Kirito", "scores": [10, 20, 30], "nested": {"x": [1, 2]}}
d.save(data, f)
var loaded = d.load(f)
String(loaded["name"]) + ":" + String(loaded["scores"][2]) + ":" + String(loaded["nested"]["x"][0])
)") == "Kirito:30:1");
    std::filesystem::remove(std::filesystem::temp_directory_path() / "kirito_dump_test.bin");

    // module-level save(value, path) (symmetric with serialize.save): dumps + write in one step
    CHECK(evalStr(vm, R"(
var d = import("dump")
var f = import("path").gettempdir() + "/kirito_dump_save.bin"
d.save([1, 2, {"k": "v"}], f)
var loaded = d.load(f)
String(loaded[2]["k"]) + ":" + String(len(loaded))
)") == "v:3");
    std::filesystem::remove(std::filesystem::temp_directory_path() / "kirito_dump_save.bin");

    // a large/deep structure
    CHECK(evalStr(vm, R"(
var d = import("dump")
var big = []
for i in range(1000):
    big.append([i, i * i])
var r = d.loads(d.dumps(big))
len(r) == 1000 and r[999][1] == 998001
)") == "True");

    // hostile / corrupt inputs throw cleanly, never crash
    CHECK_THROWS(vm.runSource("import(\"dump\").loads(\"not a dump\")"));
    CHECK_THROWS(vm.runSource("import(\"dump\").loads(\"\")"));
    CHECK_THROWS(vm.runSource("import(\"dump\").loads(\"KDMP\")"));  // header only, truncated
    CHECK_THROWS(vm.runSource("import(\"dump\").dumps(Function(x): return x)"));  // unsupported type
    CHECK_THROWS(vm.runSource("import(\"dump\").load(\"/nonexistent/path/xyz.bin\")"));

    // the binary blob is now Bytes — itself a serializable value, so it round-trips through dump
    // (unlike the old opaque Dump type, which couldn't be re-dumped)
    CHECK(evalStr(vm, R"(
var d = import("dump")
var blob = d.dumps([1, 2, 3])
d.loads(d.dumps(blob)) == blob
)") == "True");

    return RUN_TESTS();
}
