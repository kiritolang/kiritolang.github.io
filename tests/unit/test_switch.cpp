// switch/case/default: no-fallthrough dispatch, mixed-type and multi-value cases, O(1) jump table,
// control-flow propagation, and error reporting.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; } catch (const KiritoError&) { return true; }
}
static std::string errOf(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return ""; } catch (const KiritoError& e) { return e.what(); }
}

static const char* DESCRIBE =
    "var f = Function(x):\n"
    "    switch x:\n"
    "        case 1:\n            return \"one\"\n"
    "        case 2, 3:\n            return \"two-three\"\n"
    "        case \"hi\":\n            return \"greeting\"\n"
    "        case True:\n            return \"booltrue\"\n"
    "        case None:\n            return \"none\"\n"
    "        default:\n            return \"other\"\n";

int main() {
    // mixed types, multiple values per case, default fallthrough-to-default
    {
        KiritoVM vm;
        CHECK(run(vm, std::string(DESCRIBE) + "f(1)") == "one");
        CHECK(run(vm, std::string(DESCRIBE) + "f(2)") == "two-three");
        CHECK(run(vm, std::string(DESCRIBE) + "f(3)") == "two-three");
        CHECK(run(vm, std::string(DESCRIBE) + "f(\"hi\")") == "greeting");
        CHECK(run(vm, std::string(DESCRIBE) + "f(True)") == "booltrue");
        CHECK(run(vm, std::string(DESCRIBE) + "f(None)") == "none");
        CHECK(run(vm, std::string(DESCRIBE) + "f(99)") == "other");
        CHECK(run(vm, std::string(DESCRIBE) + "f([1, 2])") == "other");  // non-scalar -> default
    }
    // type-exact: case 1 (Integer) distinct from 1.0 (Float)
    {
        KiritoVM vm;
        CHECK(run(vm,
            "var g = Function(x):\n    switch x:\n"
            "        case 1:\n            return \"int\"\n"
            "        case 1.0:\n            return \"float\"\n"
            "        default:\n            return \"?\"\n"
            "String(g(1)) + String(g(1.0))") == "intfloat");
    }
    // no default + no match = no-op
    {
        KiritoVM vm;
        CHECK(run(vm, "var hit = False\nswitch 99:\n    case 1:\n        hit = True\nString(hit)") == "False");
    }
    // control flow: break/continue/return propagate out of the switch
    {
        KiritoVM vm;
        CHECK(run(vm,
            "var out = []\nfor i in range(5):\n    switch i:\n"
            "        case 3:\n            break\n        default:\n            out.append(i)\nString(out)") == "[0, 1, 2]");
        CHECK(run(vm,
            "var out = []\nfor i in range(4):\n    switch i:\n"
            "        case 1:\n            continue\n        default:\n            out.append(i)\nString(out)") == "[0, 2, 3]");
    }
    // memoized table reused across many dispatches
    {
        KiritoVM vm;
        CHECK(run(vm,
            "var c = Function(n):\n    switch n % 3:\n"
            "        case 0:\n            return \"a\"\n        case 1:\n            return \"b\"\n"
            "        default:\n            return \"c\"\n"
            "var s = \"\"\nfor i in range(6):\n    s = s + c(i)\ns") == "abcabc");
    }
    // `case` and `default` are soft keywords: still usable as ordinary identifiers outside a switch.
    {
        KiritoVM vm;
        CHECK(run(vm, "var get = Function(d, default): return default\nget({}, 7)") == "7");
        CHECK(run(vm, "var case = 5\nvar default = 3\nString(case + default)") == "8");
        CHECK(run(vm, "var d = {\"case\": 1, \"default\": 2}\nString(d[\"case\"] + d[\"default\"])") == "3");
    }

    // case labels are compile-time CONSTANT SCALARS: a constant expression over literals folds
    // (matching a runtime evaluation), while a variable / call / non-scalar / duplicate is a compile
    // error — reported at compile time, so even a never-reached bad switch does not load.
    {
        KiritoVM vm;
        CHECK(run(vm, "switch 7:\n    case 3 + 4:\n        var r = \"add\"\nr") == "add");
        CHECK(run(vm, "switch -8:\n    case -2 ** 3:\n        var r = \"p\"\nr") == "p");
        CHECK(run(vm, "switch \"ab\":\n    case \"a\" + \"b\":\n        var r = \"c\"\nr") == "c");
        CHECK(run(vm, "switch 1.5:\n    case 3 / 2:\n        var r = \"f\"\nr") == "f");   // true-div -> Float key
        // a folded label agrees with the same expression evaluated at run time
        CHECK(run(vm, "var n = 6 * 7\nswitch n:\n    case 6 * 7:\n        var r = \"ok\"\nr") == "ok");
    }
    // errors — all reported at compile time
    {
        KiritoVM vm;
        CHECK(errOf(vm, "switch 1:\n    case 1:\n        var a=1\n    case 1:\n        var b=2\n").find("duplicate switch case value") != std::string::npos);
        CHECK(errOf(vm, "switch 1:\n    case 2, 3:\n        var a=1\n    case 3:\n        var b=2\n").find("duplicate switch case value") != std::string::npos);
        CHECK(errOf(vm, "switch 1:\n    case 3 + 4:\n        var a=1\n    case 7:\n        var b=2\n").find("duplicate switch case value") != std::string::npos);  // folded dup
        CHECK(errOf(vm, "switch 1:\n    case [1]:\n        var a=1\n").find("constant scalar") != std::string::npos);       // non-scalar constant
        CHECK(errOf(vm, "var v=3\nswitch 3:\n    case v:\n        var a=1\n").find("constant scalar") != std::string::npos);  // variable label
        CHECK(errOf(vm, "var f=Function():return 1\nswitch 1:\n    case f():\n        var a=1\n").find("constant scalar") != std::string::npos);  // call label
        CHECK(errOf(vm, "switch 1:\n    case 1 / 0:\n        var a=1\n").find("division by zero") != std::string::npos);      // constant that errors
        CHECK(errOf(vm, "switch 1:\n    default:\n        var a=1\n    default:\n        var b=2\n").find("only one 'default'") != std::string::npos);
        CHECK(throws(vm, "switch 1:\n    pass\n"));
        CHECK(throws(vm, "switch 1:\n    case 1\n        var a=1\n"));
    }
    return RUN_TESTS();
}
