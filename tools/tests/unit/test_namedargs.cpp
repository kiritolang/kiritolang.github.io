// Named (keyword) arguments, default parameter values, and enforcing type annotations.
// Annotations are not hints: a param ': Type' must receive a matching instance (inheritance-aware
// for user classes) and a '-> Type' return annotation is checked on the result.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Each runSource gets a fresh module scope, so every case is a self-contained program whose last
// expression is the value we assert on.
static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    // --- named arguments + defaults on NATIVE functions (builtins and module functions) ---
    {
        KiritoVM vm;
        // builtin `round` with a signature: keyword arg and default.
        CHECK(run(vm, "round(3.14159, ndigits=2)") == "3.14");
        CHECK(run(vm, "round(2.7)") == "3");            // default ndigits=None -> Integer
        CHECK(run(vm, "round(2.71828, ndigits=3)") == "2.718");
        // module function `io.open` with a default mode.
        CHECK(run(vm,
            "var io = import(\"io\")\nvar p = import(\"path\").gettempdir() + \"/kirito_namedargs_native.txt\"\n"
            "var f = io.open(p, mode=\"w\")\ndiscard f.write(\"yo\")\nf.close()\n"
            "var g = io.open(p)\nvar s = g.read()\ng.close()\nimport(\"path\").remove(p)\ns") == "yo");
    }
    // native-signature errors: unknown keyword, duplicate, too many positional, missing required.
    {
        KiritoVM vm;
        CHECK_THROWS(vm.runSource("round(1, bogus=2)"));
        CHECK_THROWS(vm.runSource("var io = import(\"io\")\nio.open(path=\"x\", path=\"y\")"));
        CHECK_THROWS(vm.runSource("var io = import(\"io\")\nio.open(\"a\", \"b\", \"c\")"));
        CHECK_THROWS(vm.runSource("var io = import(\"io\")\nio.open()"));
    }

    // --- named arguments, any order, mixed with positional ---
    {
        KiritoVM vm;
        std::string def = "var sub = Function(a, b):\n    return a - b\n";
        CHECK(run(vm, def + "sub(10, 3)") == "7");
        CHECK(run(vm, def + "sub(a = 10, b = 3)") == "7");
        CHECK(run(vm, def + "sub(b = 3, a = 10)") == "7");   // order-independent
        CHECK(run(vm, def + "sub(10, b = 3)") == "7");        // positional then keyword
    }

    // --- default parameter values ---
    {
        KiritoVM vm;
        std::string def = "var power = Function(base, exp = 2):\n    return base ** exp\n";
        CHECK(run(vm, def + "power(5)") == "25");
        CHECK(run(vm, def + "power(5, 3)") == "125");
        CHECK(run(vm, def + "power(base = 4)") == "16");
        CHECK(run(vm, def + "power(exp = 3, base = 2)") == "8");
    }

    // --- argument-binding errors (caught at Kirito level as String messages) ---
    {
        KiritoVM vm;
        std::string def = "var f = Function(a, b):\n    return a\n";
        auto err = [&](const std::string& call) {
            return run(vm, def + "try:\n    " + call + "\ncatch as e:\n    e\n");
        };
        CHECK(err("f(1, 2, 3)").find("takes 2 positional argument") != std::string::npos);
        CHECK(err("f(1, a = 2)").find("multiple values for argument 'a'") != std::string::npos);
        CHECK(err("f(1, c = 2)").find("unexpected keyword argument 'c'") != std::string::npos);
        CHECK(err("f(1)").find("missing required argument 'b'") != std::string::npos);
        CHECK(err("f(a = 1, 2)").find("positional argument follows keyword") != std::string::npos);
    }

    // --- enforcing parameter annotations (built-in types) ---
    {
        KiritoVM vm;
        std::string def = "var keys = Function(d : Dict):\n    return len(d)\n";
        CHECK(run(vm, def + "keys({1: 2, 3: 4})") == "2");
        auto err = [&](const std::string& call) {
            return run(vm, def + "try:\n    " + call + "\ncatch as e:\n    e\n");
        };
        CHECK(err("keys([1, 2, 3])") == "argument 'd' must be Dict, got List");
        CHECK(err("keys(5)") == "argument 'd' must be Dict, got Integer");
    }

    // --- enforcing return annotations ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var half = Function(n : Integer) -> Float:\n    return n / 2\nhalf(7)") == "3.5");
        std::string bad = "var bad = Function() -> Integer:\n    return \"nope\"\n";
        CHECK(run(vm, bad + "try:\n    bad()\ncatch as e:\n    e\n")
              == "function must return Integer, got String");
    }

    // --- inheritance: a subclass instance satisfies a base-class annotation ---
    {
        KiritoVM vm;
        std::string def =
            "class Animal:\n"
            "    var speak = Function(self):\n"
            "        return \"...\"\n"
            "class Dog(Animal):\n"
            "    var speak = Function(self):\n"
            "        return \"woof\"\n"
            "var hear = Function(a : Animal):\n"
            "    return a.speak()\n";
        CHECK(run(vm, def + "hear(Dog())") == "woof");     // child passes base annotation
        CHECK(run(vm, def + "hear(Animal())") == "...");
        CHECK(run(vm, def + "try:\n    hear(42)\ncatch as e:\n    e\n").find("must be Animal, got Integer")
              != std::string::npos);
    }

    // --- 'Any' / no annotation accept everything ---
    {
        KiritoVM vm;
        std::string def = "var id = Function(x : Any):\n    return x\n";
        CHECK(run(vm, def + "id(\"s\")") == "s");
        CHECK(run(vm, def + "id([1])") == "[1]");
        CHECK(run(vm, "len([1, 2])") == "2");
    }
    // a signatured native takes keyword args by its declared parameter name, and rejects unknown ones
    {
        KiritoVM vm;
        CHECK(run(vm, "len(x = [1, 2])") == "2");  // `len`'s parameter is named `x`
        CHECK(run(vm, "try:\n    len(obj = [1, 2])\ncatch as e:\n    e\n")
                  .find("unexpected keyword argument 'obj'") != std::string::npos);
        // a keyword-aware variadic native (io.print accepts only `stream=`) rejects other keywords
        CHECK(run(vm, "var io = import(\"io\")\ntry:\n    io.print(end = 1)\ncatch as e:\n    e\n")
                  .find("unexpected keyword argument 'end'") != std::string::npos);
    }

    // --- keyword arguments on the builtin type constructors / converters ---
    {
        KiritoVM vm;
        CHECK(run(vm, "Integer(x = \"42\")") == "42");
        CHECK(run(vm, "Float(x = 3)") == "3.0");
        CHECK(run(vm, "String(x = 99)") == "99");
        CHECK(run(vm, "Bool(x = 0)") == "False");
        CHECK(run(vm, "List(iterable = range(3))") == "[0, 1, 2]");
        CHECK(run(vm, "len(Set(iterable = [1, 1, 2, 2, 3]))") == "3");
        CHECK(run(vm, "Dict(iterable = [[\"a\", 1]])[\"a\"]") == "1");
        CHECK(run(vm, "List()") == "[]");                 // no-arg still builds empty
        CHECK(run(vm, "len(Dict())") == "0");
        // unknown keyword on a converter is rejected by name
        CHECK(run(vm, "try:\n    Integer(value = 1)\ncatch as e:\n    e\n")
                  .find("unexpected keyword argument 'value'") != std::string::npos);
    }

    // --- min / max: keyword-aware variadic with `key` and `default` ---
    {
        KiritoVM vm;
        CHECK(run(vm, "min([3, 1, 2])") == "1");
        CHECK(run(vm, "max([3, 1, 2])") == "3");
        CHECK(run(vm, "min(3, 1, 2)") == "1");                       // several positionals
        CHECK(run(vm, "max(\"a\", \"bbb\", \"cc\", key = len)") == "bbb");
        CHECK(run(vm, "min([3, 1, 2], key = Function(x):\n    return -x\n)") == "3");
        CHECK(run(vm, "min([], default = \"none\")") == "none");
        CHECK(run(vm, "max([], default = -1)") == "-1");
        // empty without a default throws; an unknown keyword is rejected
        CHECK(run(vm, "try:\n    min([])\ncatch as e:\n    e\n").find("empty sequence") != std::string::npos);
        CHECK(run(vm, "try:\n    min([1], bogus = 2)\ncatch as e:\n    e\n")
                  .find("unexpected keyword argument 'bogus'") != std::string::npos);
    }

    // --- math.prod accepts keyword args (iterable + start default 1) ---
    {
        KiritoVM vm;
        CHECK(run(vm, "import(\"math\").prod(iterable = [2, 3, 4])") == "24");
        CHECK(run(vm, "import(\"math\").prod([2, 3, 4], start = 10)") == "240");
    }

    // --- out-of-order keywords on ASYMMETRIC natives bind strictly by name (a swap would differ) ---
    {
        KiritoVM vm;
        CHECK(run(vm, "divmod(b = 5, a = 17)") == "[3, 2]");      // not divmod(5, 17) == [0, 5]
        CHECK(run(vm, "divmod(b = 17, a = 5)") == "[0, 5]");
        CHECK(run(vm, "pow(mod = 5, exp = 3, base = 2)") == "3"); // 2**3 % 5 == 3, fully shuffled
        CHECK(run(vm, "round(ndigits = 1, x = 2.345)") == "2.3");
        CHECK(run(vm, "format(spec = \"03d\", value = 7)") == "007");
        CHECK(run(vm, "isinstance(type = \"String\", value = \"hi\")") == "True");
        CHECK(run(vm, "import(\"math\").log(base = 2, x = 8)") == "3.0");
        CHECK(run(vm, "import(\"math\").prod(start = 2, iterable = [3, 4])") == "24");
        // positional prefix, then a reversed keyword remainder
        CHECK(run(vm, "pow(2, mod = 5, exp = 3)") == "3");
        CHECK(run(vm, "divmod(17, b = 5)") == "[3, 2]");
    }

    return RUN_TESTS();
}
