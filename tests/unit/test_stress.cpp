// Stress / adversarial / edge-case suite. Locks in fixes for a class of "optional argument silently
// ignored" string-method bugs (strip/split/replace/find/index/count/startswith/endswith), List `*`
// repetition, and the format `#` alternate form — and pins down robustness on cycles, overflow,
// NaN, switch type-matching, closures, and named-argument binding that adversarial probing verified.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }
static std::string err(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return ""; } catch (const KiritoError& e) { return e.what(); }
}

int main() {
    // ===== string methods must honor their optional arguments (regressions) =====
    {
        KiritoVM vm;
        // strip(chars)
        CHECK(run(vm, "\"xxabcxx\".strip(\"x\")") == "abc");
        CHECK(run(vm, "\"xyabcyx\".strip(\"xy\")") == "abc");
        CHECK(run(vm, "\"xxabc\".lstrip(\"x\")") == "abc");
        CHECK(run(vm, "\"abcxx\".rstrip(\"x\")") == "abc");
        CHECK(run(vm, "\"  abc  \".strip()") == "abc");
        // split(sep, maxsplit) and split(None, maxsplit)
        CHECK(run(vm, "\",\".join(\"a,b,c\".split(\",\", 1))") == "a,b,c");
        CHECK(run(vm, "String(len(\"a,b,c\".split(\",\", 1)))") == "2");
        CHECK(run(vm, "\"a,b,c\".split(\",\", 1)[1]") == "b,c");
        CHECK(run(vm, "\"a b c d\".split(None, 1)[1]") == "b c d");
        CHECK(run(vm, "String(len(\"a,b,c\".split(\",\", 0)))") == "1");
        // replace(old, new, count)
        CHECK(run(vm, "\"aaaa\".replace(\"a\", \"b\", 2)") == "bbaa");
        CHECK(run(vm, "\"aaaa\".replace(\"a\", \"b\")") == "bbbb");
        CHECK(run(vm, "\"aaaa\".replace(\"a\", \"b\", 0)") == "aaaa");
        // find / index / rfind / rindex with start[, end]
        CHECK(run(vm, "String(\"abcabc\".find(\"a\", 1))") == "3");
        CHECK(run(vm, "String(\"abcabc\".find(\"bc\", 0, 3))") == "1");
        CHECK(run(vm, "String(\"abcabc\".find(\"c\", 0, 2))") == "-1");
        CHECK(run(vm, "String(\"abcabc\".rfind(\"a\", 0, 3))") == "0");
        CHECK(run(vm, "String(\"abcabc\".index(\"b\", 2))") == "4");
        CHECK(err(vm, "\"abcabc\".index(\"a\", 4)").find("not found") != std::string::npos);
        // count with [start, end]
        CHECK(run(vm, "String(\"abcabc\".count(\"a\", 1))") == "1");
        CHECK(run(vm, "String(\"aaaa\".count(\"a\", 1, 3))") == "2");
        // startswith / endswith with [start, end]
        CHECK(run(vm, "String(\"abcabc\".startswith(\"bc\", 1))") == "True");
        CHECK(run(vm, "String(\"abcabc\".startswith(\"ab\", 1))") == "False");
        CHECK(run(vm, "String(\"abcabc\".endswith(\"bc\", 0, 3))") == "True");
        CHECK(run(vm, "String(\"abcabc\".endswith(\"ab\", 0, 3))") == "False");
        // unicode: start is a code-point index, not a byte offset
        CHECK(run(vm, "String(\"héllo héllo\".find(\"llo\", 5))") == "8");
    }

    // ===== List `*` repetition (regression: was unimplemented) =====
    {
        KiritoVM vm;
        CHECK(run(vm, "[1, 2] * 3") == "[1, 2, 1, 2, 1, 2]");
        CHECK(run(vm, "3 * [1, 2]") == "[1, 2, 1, 2, 1, 2]");
        CHECK(run(vm, "[1, 2] * 0") == "[]");
        CHECK(run(vm, "[1, 2] * -3") == "[]");
        // repetition shares element handles
        CHECK(run(vm, "var x = [[0]] * 3\nx[0].append(9)\nString(x)") == "[[0, 9], [0, 9], [0, 9]]");
        // guarded against absurd counts
        CHECK(err(vm, "[0] * 100000000000").find("too large") != std::string::npos);
    }

    // ===== format `#` alternate form (regression: was parsed but ignored) =====
    {
        KiritoVM vm;
        CHECK(run(vm, "format(255, \"#x\")") == "0xff");
        CHECK(run(vm, "format(255, \"#X\")") == "0XFF");
        CHECK(run(vm, "format(8, \"#o\")") == "0o10");
        CHECK(run(vm, "format(5, \"#b\")") == "0b101");
        CHECK(run(vm, "format(255, \"#08x\")") == "0x0000ff");
        CHECK(run(vm, "format(-255, \"#x\")") == "-0xff");
        CHECK(run(vm, "format(255, \"x\")") == "ff");
    }

    // ===== integer overflow: defined wraparound, never UB/crash =====
    {
        KiritoVM vm;
        std::string MIN = "var MIN = -9223372036854775807 - 1\n";
        CHECK(run(vm, MIN + "String(MIN // -1)") == "-9223372036854775808");  // wraps, no crash
        CHECK(run(vm, MIN + "String(MIN % -1)") == "0");
        CHECK(run(vm, MIN + "String(-MIN)") == "-9223372036854775808");
        CHECK(run(vm, MIN + "String(abs(MIN))") == "-9223372036854775808");
    }

    // ===== NaN / float key semantics (internally consistent, equality-based) =====
    {
        KiritoVM vm;
        std::string nan = "var nan = import(\"math\").nan\n";
        CHECK(run(vm, nan + "String(nan == nan)") == "False");
        CHECK(run(vm, nan + "String(nan < 1 or nan > 1)") == "False");
        // Integer and Float that compare equal collapse to one dict key
        CHECK(run(vm, "var d = {}\nd[1] = \"i\"\nd[1.0] = \"f\"\nString(len(d))") == "1");
        // 0.0 and -0.0 are the same key
        CHECK(run(vm, "var d = {}\nd[0.0] = 1\nd[-1.0 * 0.0] = 2\nString(len(d))") == "1");
    }

    // ===== cyclic structures must terminate (str / == / in) =====
    {
        KiritoVM vm;
        CHECK(run(vm, "var a = [1, 2]\na.append(a)\nString(a)") == "[1, 2, [...]]");
        CHECK(run(vm, "var a = []\na.append(a)\nString(a == a)") == "True");
        CHECK(run(vm, "var d = {}\nd[\"k\"] = d\nString(d)") == "{'k': {...}}");
    }

    // ===== switch: exact type matching, single subject evaluation =====
    {
        KiritoVM vm;
        std::string sw =
            "var f = Function(x):\n"
            "    switch x:\n"
            "        case 1:\n            return \"int\"\n"
            "        case True:\n            return \"bool\"\n"
            "        case 1.0:\n            return \"float\"\n"
            "        default:\n            return \"other\"\n";
        CHECK(run(vm, sw + "f(1)") == "int");
        CHECK(run(vm, sw + "f(True)") == "bool");
        CHECK(run(vm, sw + "f(1.0)") == "float");
        CHECK(run(vm, sw + "f(2)") == "other");
        // subject evaluated exactly once
        CHECK(run(vm, "var n = 0\nvar g = Function():\n    n = n + 1\n    return 2\n"
                      "switch g():\n    case 2:\n        pass\nString(n)") == "1");
    }

    // ===== closures: late binding of loop var; independent instances =====
    {
        KiritoVM vm;
        CHECK(run(vm, "var fns = []\nfor i in range(3):\n    fns.append(Function(): return i)\n"
                      "String([fns[0](), fns[1](), fns[2]()])") == "[2, 2, 2]");
        CHECK(run(vm, "var make = Function(n): return Function(): return n\n"
                      "String([make(10)(), make(20)()])") == "[10, 20]");
    }

    // ===== named-argument binding error paths (Kirito + native) =====
    {
        KiritoVM vm;
        std::string g = "var g = Function(a, b): return a - b\n";
        CHECK(run(vm, g + "String(g(b=3, a=10))") == "7");
        CHECK(err(vm, g + "g(10, a=1)").find("multiple values for argument 'a'") != std::string::npos);
        CHECK(err(vm, g + "g(1, 2, 3)").find("positional") != std::string::npos);
        CHECK(err(vm, g + "g(1)").find("missing required argument 'b'") != std::string::npos);
        CHECK(err(vm, "round(1, bogus=2)").find("unexpected keyword argument 'bogus'") != std::string::npos);
    }

    // ===== List.index honors the optional [start[, end]] window =====
    {
        KiritoVM vm;
        CHECK(run(vm, "String([1, 2, 1, 2].index(1, 1))") == "2");
        CHECK(run(vm, "String([1, 2, 1, 2].index(2, 2))") == "3");
        CHECK(run(vm, "String([1, 2, 1, 2].index(2, -2))") == "3");
        CHECK(err(vm, "[1, 2, 1, 2].index(1, 1, 2)").find("not in List") != std::string::npos);
    }

    // ===== collections respect a user class's _eq_ (in / index / count / remove) =====
    {
        KiritoVM vm;
        std::string P =
            "class P:\n"
            "    var _init_ = Function(self, v):\n        self.v = v\n"
            "    var _eq_ = Function(self, o):\n        return self.v == o.v\n";
        CHECK(run(vm, P + "String(P(1) == P(1))") == "True");
        CHECK(run(vm, P + "String(P(1) in [P(2), P(1)])") == "True");
        CHECK(run(vm, P + "String(P(9) in [P(2), P(1)])") == "False");
        CHECK(run(vm, P + "String([P(2), P(1)].index(P(1)))") == "1");
        CHECK(run(vm, P + "String([P(1), P(1), P(2)].count(P(1)))") == "2");
        CHECK(run(vm, P + "var L = [P(1), P(2), P(1)]\nL.remove(P(1))\nString(len(L))") == "2");
    }

    // ===== sorted / min / max / < respect a user class's _lt_ =====
    {
        KiritoVM vm;
        std::string P =
            "class P:\n"
            "    var _init_ = Function(self, v):\n        self.v = v\n"
            "    var _lt_ = Function(self, o):\n        return self.v < o.v\n"
            "    var _str_ = Function(self):\n        return \"P\" + String(self.v)\n";
        CHECK(run(vm, P + "String(sorted([P(3), P(1), P(2)]))") == "[P1, P2, P3]");
        CHECK(run(vm, P + "String(min([P(3), P(1), P(2)]))") == "P1");
        CHECK(run(vm, P + "String(max([P(3), P(1), P(2)]))") == "P3");
        CHECK(run(vm, P + "String(P(1) < P(2))") == "True");
        CHECK(run(vm, P + "String(sorted([P(3), P(1), P(2)], reverse=True))") == "[P3, P2, P1]");
    }

    // ===== resource guards: bounded, throw instead of OOM =====
    {
        KiritoVM vm;
        CHECK(err(vm, "\"x\" * 100000000000").find("too large") != std::string::npos);
        CHECK(err(vm, "len(range(100000000000))").find("too large") != std::string::npos);
        CHECK(run(vm, "\"x\" * -5") == "");
    }

    return RUN_TESTS();
}
