// The container `apply(fn)` method — a new same-type container with `fn` mapped over the elements,
// mirroring tensor.apply. Defined on List, Set, Dict (over values), Bytes (over bytes), and String
// (over characters). Also checks the containers now describe their methods under `inspect`.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string ev(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }

int main() {
    KiritoVM vm;
    vm.installStandardLibrary();

    // --- List.apply -> List ---------------------------------------------------------------------
    CHECK(ev(vm, "[1, 2, 3, 4].apply(Function(x): return x * x)") == "[1, 4, 9, 16]");
    CHECK(ev(vm, "type([1].apply(Function(x): return x))") == "List");
    CHECK(ev(vm, "[].apply(Function(x): return x)") == "[]");                 // empty
    CHECK(ev(vm, "[1, 2, 3].apply(Function(x): return x * 2)") == "[2, 4, 6]");  // order preserved
    CHECK(ev(vm, "[\"a\", \"b\"].apply(Function(s): return s + \"!\")") == "['a!', 'b!']");
    // the source list is unchanged (apply returns a new List)
    CHECK(ev(vm, "var xs = [1, 2, 3]\nvar ys = xs.apply(Function(x): return x + 10)\nString(xs) + \" \" + String(ys)") == "[1, 2, 3] [11, 12, 13]");
    // a closure captured in the mapped function
    CHECK(ev(vm, R"(
var scale = Function(k): return Function(x): return x * k
[1, 2, 3].apply(scale(10))
)") == "[10, 20, 30]");

    // --- Set.apply -> Set (collisions collapse) --------------------------------------------------
    CHECK(ev(vm, "type({1, 2, 3}.apply(Function(x): return x))") == "Set");
    CHECK(ev(vm, "len({1, 2, 3, 4}.apply(Function(x): return x % 2))") == "2");   // {0, 1}
    CHECK(ev(vm, "{5}.apply(Function(x): return x + 1)") == "{6}");

    // --- Dict.apply -> Dict (values mapped, keys kept) -------------------------------------------
    CHECK(ev(vm, "type({\"a\": 1}.apply(Function(v): return v))") == "Dict");
    CHECK(ev(vm, "var d = {\"a\": 1, \"b\": 2}.apply(Function(v): return v * 100)\nd[\"a\"] + d[\"b\"]") == "300");
    CHECK(ev(vm, R"(
var d = {"x": 1, "y": 2, "z": 3}
var keys_same = True
for k in d.apply(Function(v): return v).keys():
    if not (k in d):
        keys_same = False
keys_same
)") == "True");

    // --- Bytes.apply -> Bytes (over byte values 0..255) ------------------------------------------
    CHECK(ev(vm, "Bytes([1, 2, 3]).apply(Function(b): return b * 2)") == "b'\\x02\\x04\\x06'");
    CHECK(ev(vm, "type(Bytes([1]).apply(Function(b): return b))") == "Bytes");
    CHECK(ev(vm, "Bytes([72, 73]).apply(Function(b): return b + 32).decode()") == "hi");   // upper->lower
    CHECK_THROWS(vm.runSource("Bytes([1]).apply(Function(b): return 999)"));   // result out of byte range
    CHECK_THROWS(vm.runSource("Bytes([1]).apply(Function(b): return -1)"));

    // --- String.apply -> String (over characters) ------------------------------------------------
    CHECK(ev(vm, "\"hello\".apply(Function(c): return c.upper())") == "HELLO");
    CHECK(ev(vm, "type(\"x\".apply(Function(c): return c))") == "String");
    CHECK(ev(vm, "\"abc\".apply(Function(c): return c + c)") == "aabbcc");   // a char may expand
    CHECK(ev(vm, "\"héllo\".apply(Function(c): return c)") == "héllo");      // Unicode code points
    CHECK_THROWS(vm.runSource("\"x\".apply(Function(c): return 42)"));       // result must be a String

    // --- errors propagate out of apply -----------------------------------------------------------
    CHECK_THROWS(vm.runSource("[1, 2, 3].apply(Function(x): throw \"boom\")"));
    CHECK_THROWS(vm.runSource("[1].apply(42)"));                              // not callable

    // --- GC stress: a mapping function that allocates on every call (exercises rooting) ----------
    CHECK(ev(vm, R"(
var xs = []
var i = 0
while i < 200:
    xs.append(i)
    i = i + 1
var ys = xs.apply(Function(x): return [x, x * x, String(x)])   # each result is a fresh List
len(ys) == 200 and ys[150][1] == 22500 and ys[199][2] == "199"
)") == "True");
    CHECK(ev(vm, R"(
var s = "abcdefghij" * 20
var out = s.apply(Function(c): return c + "-")   # each call allocates a new String
len(out) == 400
)") == "True");

    // --- inspect now lists the methods (including apply) -----------------------------------------
    CHECK(ev(vm, "inspect([1]).find(\"apply(fn) -> List\") >= 0") == "True");
    CHECK(ev(vm, "inspect({1}).find(\"apply(fn) -> Set\") >= 0") == "True");
    CHECK(ev(vm, "inspect({\"a\": 1}).find(\"apply(fn) -> Dict\") >= 0") == "True");
    CHECK(ev(vm, "inspect(Bytes([1])).find(\"apply(fn) -> Bytes\") >= 0") == "True");
    CHECK(ev(vm, "inspect([1]).find(\"append(item)\") >= 0") == "True");      // other methods listed too

    return RUN_TESTS();
}
