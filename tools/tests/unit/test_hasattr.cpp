#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

// hasattr(obj, name) -> Bool: existence of an attribute/method on any value. True even when the value
// is None; False when absent; a non-String name throws. Mirrors `obj.name` resolution exactly.
int main() {
    KiritoVM vm;

    // --- instances: attribute existence, including the None case ---
    const char* cls = R"(
class Box:
    var _init_ = Function(self, v):
        self.value = v
        self.empty = None
        self._priv = 1
    var m = Function(self):
        return self.value
)";
    CHECK(evalStr(vm, std::string(cls) + "hasattr(Box(3), \"value\")") == "True");
    CHECK(evalStr(vm, std::string(cls) + "hasattr(Box(3), \"empty\")") == "True");   // None still exists
    CHECK(evalStr(vm, std::string(cls) + "hasattr(Box(3), \"m\")") == "True");       // method
    CHECK(evalStr(vm, std::string(cls) + "hasattr(Box(3), \"_init_\")") == "True");  // dunder
    CHECK(evalStr(vm, std::string(cls) + "hasattr(Box(3), \"missing\")") == "False");
    CHECK(evalStr(vm, std::string(cls) + "hasattr(Box(3), \"_priv\")") == "True");   // private EXISTS

    // the headline guarantee: None-valued attr is distinguishable from an absent one
    CHECK(evalStr(vm, std::string(cls) +
        "var b = Box(3)\nhasattr(b, \"empty\") and not hasattr(b, \"nope\")") == "True");

    // --- a Kirito class exposes no member access, so hasattr(Class, ...) is uniformly False ---
    CHECK(evalStr(vm, std::string(cls) + "hasattr(Box, \"m\")") == "False");
    CHECK(evalStr(vm, std::string(cls) + "hasattr(Box, \"value\")") == "False");

    // --- built-in primitives and their methods ---
    CHECK(evalStr(vm, "hasattr(\"hi\", \"upper\")") == "True");
    CHECK(evalStr(vm, "hasattr(\"hi\", \"zzz\")") == "False");
    CHECK(evalStr(vm, "hasattr(5, \"compare\")") == "True");
    CHECK(evalStr(vm, "hasattr([1], \"append\")") == "True");
    CHECK(evalStr(vm, "hasattr({\"a\": 1}, \"keys\")") == "True");
    CHECK(evalStr(vm, "hasattr(Set([1]), \"add\")") == "True");
    CHECK(evalStr(vm, "hasattr(Bytes([1]), \"hex\")") == "True");
    CHECK(evalStr(vm, "hasattr(None, \"anything\")") == "False");
    CHECK(evalStr(vm, "hasattr(3.5, \"compare\")") == "True");

    // --- modules: native and .ki ---
    CHECK(evalStr(vm, "hasattr(import(\"math\"), \"pi\")") == "True");
    CHECK(evalStr(vm, "hasattr(import(\"math\"), \"sqrt\")") == "True");
    CHECK(evalStr(vm, "hasattr(import(\"math\"), \"nope\")") == "False");
    CHECK(evalStr(vm, "hasattr(import(\"itertools\"), \"chain\")") == "True");   // .ki module
    CHECK(evalStr(vm, "hasattr(import(\"itertools\"), \"nope\")") == "False");

    // --- native objects ---
    CHECK(evalStr(vm, "hasattr(import(\"random\").Random(1), \"choices\")") == "True");
    CHECK(evalStr(vm, "hasattr(import(\"random\").Random(1), \"zzz\")") == "False");

    // --- functions have no attributes ---
    CHECK(evalStr(vm, "hasattr(Function(x): return x, \"whatever\")") == "False");

    // --- consistency invariant for PUBLIC members: hasattr(o, n) == (o.n resolves) ---
    CHECK(evalStr(vm, std::string(cls) + R"(
var got = True
var b = Box(1)
var check = Function(name, thunk):
    var access = False
    try:
        discard thunk()
        access = True
    catch:
        access = False
    if hasattr(b, name) != access:
        got = False
check("value", Function(): return b.value)
check("empty", Function(): return b.empty)
check("m", Function(): return b.m)
check("missing", Function(): return b.missing)
got
)") == "True");

    // --- existence, not accessibility: a private member EXISTS (hasattr True) yet cannot be READ
    //     from outside the class (direct access throws). This is the deliberate divergence. ---
    CHECK(evalStr(vm, std::string(cls) + "hasattr(Box(1), \"_priv\")") == "True");
    CHECK_THROWS(vm.runSource(std::string(cls) + "Box(1)._priv\n"));

    // --- error hardening: name must be a String; strict arity ---
    CHECK_THROWS(vm.runSource("hasattr(5)\n"));                 // too few
    CHECK_THROWS(vm.runSource("hasattr(5, \"a\", \"b\")\n"));   // too many
    CHECK_THROWS(vm.runSource("hasattr(5, 42)\n"));             // non-String name
    CHECK_THROWS(vm.runSource("hasattr(5, None)\n"));           // None name
    CHECK_THROWS(vm.runSource("hasattr(5, [\"x\"])\n"));        // List name

    // hasattr is a resolvable builtin name (no NameError when used bare in a function)
    CHECK(evalStr(vm, R"(
var f = Function(o):
    return hasattr(o, "upper")
f("x")
)") == "True");

    return RUN_TESTS();
}
