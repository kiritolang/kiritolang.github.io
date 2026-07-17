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

    // A user-class instance CARRIES its class: a FRESH VM that never defined the class rebuilds it from
    // the blob — no import needed. Two independent VMs share only a temp file, so there is no hidden
    // dependency on VM1's class registry. (Both compute the same process-independent temp path.)
    {
        const char* saveScript =
            "var d = import(\"dump\")\n"
            "class Carry:\n"
            " var _init_ = Function(self, n):\n"
            "  self.n = n\n"
            " var dbl = Function(self): return self.n * 2\n"
            "var p = import(\"path\").gettempdir() + \"/kirito_inst_carry.bin\"\n"
            "d.save(Carry(21), p)\n";
        { KiritoVM v1; v1.runSource(saveScript); }
        KiritoVM v2;  // fresh: Carry was never defined here
        const char* loadScript =
            "var d = import(\"dump\")\n"
            "var p = import(\"path\").gettempdir() + \"/kirito_inst_carry.bin\"\n"
            "var w = d.load(p)\n"
            "String(w.n) + \":\" + String(w.dbl())\n";
        CHECK(v2.stringify(v2.runSource(loadScript)) == "21:42");
        std::filesystem::remove(std::filesystem::temp_directory_path() / "kirito_inst_carry.bin");
    }

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
    // A native/built-in function is not serializable (a Kirito-defined Function now IS — see below).
    CHECK_THROWS(vm.runSource("import(\"dump\").dumps(len)"));
    CHECK_THROWS(vm.runSource("import(\"dump\").load(\"/nonexistent/path/xyz.bin\")"));

    // Kirito functions serialize by default: a plain function, a closure (captured value travels by
    // value), and self-recursion (the function references itself) all round-trip through the binary.
    CHECK(evalStr(vm, R"(
var d = import("dump")
var sq = Function(x): return x * x
d.loads(d.dumps(sq))(9)
)") == "81");
    CHECK(evalStr(vm, R"(
var d = import("dump")
var base = 100
var add = Function(x): return x + base
d.loads(d.dumps(add))(5)
)") == "105");
    CHECK(evalStr(vm, R"(
var d = import("dump")
var fib = Function(n): return n if n < 2 else fib(n-1) + fib(n-2)
d.loads(d.dumps(fib))(10)
)") == "55");
    // A class round-trips, and so does an instance — carrying its class, so no import is needed.
    CHECK(evalStr(vm, R"(
var d = import("dump")
class Pt:
 var _init_ = Function(self, x, y):
  self.x = x
  self.y = y
 var norm2 = Function(self): return self.x*self.x + self.y*self.y
d.loads(d.dumps(Pt))(x=3, y=4).norm2()
)") == "25");
    CHECK(evalStr(vm, R"(
var d = import("dump")
class Pt2:
 var _init_ = Function(self, v): self.v = v
 var twice = Function(self): return self.v * 2
d.loads(d.dumps(Pt2(21))).twice()
)") == "42");

    // the binary blob is now Bytes — itself a serializable value, so it round-trips through dump
    // (unlike the old opaque Dump type, which couldn't be re-dumped)
    CHECK(evalStr(vm, R"(
var d = import("dump")
var blob = d.dumps([1, 2, 3])
d.loads(d.dumps(blob)) == blob
)") == "True");

    return RUN_TESTS();
}
