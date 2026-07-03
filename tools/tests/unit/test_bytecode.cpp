// Behavioural + stress tests for the bytecode engine — Kirito's sole execution engine. Programs are
// compiled to a Proto and run on the stack VM; these pin specific results, exercise the operand-stack
// GC integration, and confirm errors surface correctly (including compile-time diagnostics).
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run a source chunk on a fresh VM; return the stringified last value.
static std::string run(const std::string& src) {
    KiritoVM vm;
    return vm.stringify(vm.runSource(src));
}
static std::string val(const std::string& src) { return run(src); }

int main() {
    // --- arithmetic, precedence, division kinds, wraparound ---
    CHECK(val("1 + 2 * 3") == "7");
    CHECK(val("2 ** 3 ** 2") == "512");      // right-assoc
    CHECK(val("-2 ** 2") == "-4");           // ** binds tighter than unary minus
    CHECK(val("7 // 2") == "3");
    CHECK(val("-7 // 2") == "-4");           // floor division
    CHECK(val("-7 % 3") == "2");             // modulo sign follows divisor
    CHECK(val("4 / 2") == "2.0");            // / always Float
    CHECK(val("1 / 2") == "0.5");

    // --- comparisons, equality across types, logical (short-circuit), conditional ---
    CHECK(val("1 < 2 and 2 <= 2") == "True");
    CHECK(val("3 > 4 or not False") == "True");
    CHECK(val("1 == \"x\"") == "False");     // mismatched-type equality never throws
    CHECK(val("\"hi\" if 3 > 2 else \"lo\"") == "hi");
    CHECK(val("0 and 1 // 0") == "0");   // RHS not evaluated -> no NameError

    // --- strings, f-strings, format specs ---
    CHECK(val("\"AB\" * 3 + \"!\"") == "ABABAB!");
    CHECK(val("var n=\"Kirito\"\nvar p=3.14159\nf\"{n}:{p:.2f}:{255:#x}:{42:05d}\"") == "Kirito:3.14:0xff:00042");
    CHECK(val("\"Hello\".lower().upper()") == "HELLO");

    // --- var / assignment / rebinding ---
    CHECK(val("var x = 10\nx = x + 5\nx") == "15");

    // --- collections: list/set/dict literals, indexing, slicing, membership, methods ---
    CHECK(val("var xs=[1,2,3,4]\nxs.append(9)\nxs") == "[1, 2, 3, 4, 9]");
    CHECK(val("[1,2,3,4,5][1:4]") == "[2, 3, 4]");
    CHECK(val("[1,2,3,4,5][::2]") == "[1, 3, 5]");
    CHECK(val("var d={\"a\":1}\nd[\"b\"]=2\nd[\"a\"]+d[\"b\"]") == "3");
    CHECK(val("\"b\" in {\"a\":1,\"b\":2}") == "True");
    CHECK(val("3 in {1,2,3}") == "True");

    // --- control flow: while/for, break/continue, nested ---
    CHECK(val("var i=0\nvar t=0\nwhile i<10:\n    i=i+1\n    if i==3:\n        continue\n    if i==7:\n        break\n    t=t+i\nt") == "18");
    CHECK(val("var s=0\nfor n in range(100):\n    s=s+n\ns") == "4950");

    // --- unpacking (var / assign / for / star) ---
    CHECK(val("var a,b=[1,2]\na,b=b,a\n[a,b]") == "[2, 1]");
    CHECK(val("var first,*rest=[1,2,3,4]\n[first, rest]") == "[1, [2, 3, 4]]");
    CHECK(val("var d={\"a\":1,\"b\":2}\nvar s=0\nfor k,v in d.items():\n    s=s+v\ns") == "3");

    // --- switch: exact type matching (case 1 != case 1.0), default, multi-value, duplicate error ---
    CHECK(val("var r=\"?\"\nswitch 2:\n    case 1:\n        r=\"a\"\n    case 2,3:\n        r=\"b\"\n    default:\n        r=\"d\"\nr") == "b");
    CHECK(val("var r=\"x\"\nswitch 1:\n    case 1.0:\n        r=\"f\"\n    default:\n        r=\"exact\"\nr") == "exact");

    // --- functions: closures, defaults, recursion, keyword args ---
    CHECK(val("var sq=Function(v): return v*v\nsq(7)") == "49");
    CHECK(val("var add=Function(a, b=100): return a+b\n[add(1), add(1,2)]") == "[101, 3]");
    CHECK(val("var fac=Function(n):\n    if n<=1:\n        return 1\n    return n*fac(n-1)\nfac(6)") == "720");
    CHECK(val("var make=Function(base):\n    return Function(x): return x+base\nmake(10)(5)") == "15");
    CHECK(val("var f=Function(a,b,c): return a*100+b*10+c\nf(c=3,a=1,b=2)") == "123");

    // --- classes: inheritance, _super_, operators, privates, getitem/setitem/len ---
    CHECK(val("class P:\n    var _init_=Function(self,x,y): \n        self.x=x\n        self.y=y\n    var _add_=Function(self,o): return P(self.x+o.x, self.y+o.y)\n    var _str_=Function(self): return f\"({self.x},{self.y})\"\nString(P(1,2)+P(3,4))") == "(4,6)");
    CHECK(val("class A:\n    var who=Function(self): return \"A\"\nclass B(A):\n    var who=Function(self): return self._super_().who()+\"B\"\nB().who()") == "AB");

    // --- exceptions: catch value, typed catch, finally semantics (incl. handler re-throw runs finally) ---
    CHECK(val("try:\n    throw \"boom\"\ncatch as e:\n    e") == "boom");
    CHECK(val("try:\n    var x=1/0\ncatch as e:\n    \"caught\"") == "caught");
    CHECK(val("var f=Function():\n    try:\n        return 1\n    finally:\n        return 2\nf()") == "2");
    CHECK(val("var o=[]\ntry:\n    try:\n        throw \"x\"\n    catch as e:\n        o.append(\"c\")\n        throw \"r\"\n    finally:\n        o.append(\"f\")\ncatch as e:\n    o.append(e)\no") == "['c', 'f', 'r']");

    // --- with: enter/exit on every path ---
    CHECK(val("class CM:\n    var _enter_=Function(self): return 5\n    var _exit_=Function(self): return None\nvar r=0\nwith CM() as c:\n    r=c\nr") == "5");

    // --- builtins over iterables/callables (calls inside bytecode) ---
    CHECK(val("sum([1,2,3,4,5])") == "15");
    CHECK(val("sorted([3,1,2])") == "[1, 2, 3]");
    CHECK(val("List(range(3))") == "[0, 1, 2]");

    // --- bare-comma packing -> List ---
    CHECK(val("var t = 1, 2, 3\nt") == "[1, 2, 3]");

    // --- behavioural pins on bigger loops ---
    CHECK(val("var z=0\nfor i in range(1000):\n    z=z+i\nz") == "499500");

    // --- GC integration: frequent collections must not reap live operand-stack values ---
    {
        KiritoVM vm;
        vm.setGcThreshold(64);  // force frequent collections during the loop
        Handle h = vm.runSource(
            "var total = 0\n"
            "for i in range(500):\n"
            "    var pair = [i, i*2]\n"
            "    var d = {\"k\": pair}\n"
            "    total = total + d[\"k\"][1]\n"
            "total");
        CHECK(vm.arena().deref(h).kind() == ValueKind::Integer);
        CHECK(static_cast<const IntVal&>(vm.arena().deref(h)).value() == 249500);  // sum of 2*i, i in 0..499
    }

    // --- runtime errors thrown correctly ---
    {
        KiritoVM vm;
        CHECK_THROWS(vm.runSource("undefined_name + 1"));
        CHECK_THROWS(vm.runSource("var d = {}\nd[\"missing\"]"));
        CHECK_THROWS(vm.runSource("[1,2,3] + 5"));
    }

    // --- compile-time diagnostics (the compiler throws these like the parser does) ---
    {
        KiritoVM vm;
        CHECK_THROWS(vm.runSource("1 + 2 = 3"));        // invalid assignment target
        CHECK_THROWS(vm.runSource("var f=Function(a,b): return a\nf(a=1, 2)"));  // positional after keyword
        CHECK_THROWS(vm.runSource("nonexistent_name"));  // undefined name -> compile-time error
    }

    // --- uncaught throw/assert surfaces as an error ---
    {
        KiritoVM vm;
        CHECK_THROWS(vm.runSource("assert 1 == 2"));
        CHECK_THROWS(vm.runSource("throw \"boom\""));
    }

    return RUN_TESTS();
}
