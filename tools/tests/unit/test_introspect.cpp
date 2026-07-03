// inspect() introspection and the new core builtins (all/any/reversed/divmod/isinstance/ord/chr/
// bin/oct/hex/pow).
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    // --- new builtins ---
    {
        KiritoVM vm;
        CHECK(run(vm, "String(all([1, 2, 3]))") == "True");
        CHECK(run(vm, "String(all([1, 0]))") == "False");
        CHECK(run(vm, "String(all([]))") == "True");
        CHECK(run(vm, "String(any([0, 0, 1]))") == "True");
        CHECK(run(vm, "String(any([]))") == "False");
        CHECK(run(vm, "String(reversed([1, 2, 3]))") == "[3, 2, 1]");
        CHECK(run(vm, "String(divmod(17, 5))") == "[3, 2]");
        CHECK(run(vm, "String(divmod(-7, 3))") == "[-3, 2]");  // floor semantics
        CHECK(run(vm, "ord(\"A\")") == "65");
        CHECK(run(vm, "ord(\"é\")") == "233");               // é (literal UTF-8)
        CHECK(run(vm, "chr(65)") == "A");
        CHECK(run(vm, "chr(233)") == "é");
        CHECK(run(vm, "ord(chr(19990))") == "19990");         // round-trip a CJK code point
        CHECK(run(vm, "bin(10)") == "0b1010");
        CHECK(run(vm, "bin(-10)") == "-0b1010");
        CHECK(run(vm, "oct(64)") == "0o100");
        CHECK(run(vm, "hex(255)") == "0xff");
        CHECK(run(vm, "hex(0)") == "0x0");
        CHECK(run(vm, "pow(2, 10)") == "1024");
        CHECK(run(vm, "pow(3, 4, 5)") == "1");                // 81 mod 5
        CHECK(run(vm, "String(pow(2, -1))") == "0.5");        // 2-arg falls back to numeric pow
    }

    // --- isinstance (kind names + inheritance) ---
    {
        KiritoVM vm;
        CHECK(run(vm, "String(isinstance(5, \"Integer\"))") == "True");
        CHECK(run(vm, "String(isinstance(5, \"String\"))") == "False");
        CHECK(run(vm, "String(isinstance([1], \"List\"))") == "True");
        CHECK(run(vm,
            "class A:\n    var f = Function(self):\n        return 1\n"
            "class B(A):\n    var g = Function(self):\n        return 2\n"
            "String(isinstance(B(), \"A\"))") == "True");
        // passing the class value itself works too
        CHECK(run(vm,
            "class A:\n    var f = Function(self):\n        return 1\n"
            "String(isinstance(A(), A))") == "True");
    }

    // --- inspect: class shows methods + annotations, hides privates ---
    {
        KiritoVM vm;
        std::string out = run(vm,
            "class Pt:\n"
            "    var _init_ = Function(self, x : Integer, y : Integer):\n"
            "        self.x = x\n"
            "        self._hidden = 0\n"
            "    var dist = Function(self) -> Float:\n"
            "        return 0.0\n"
            "inspect(Pt)");
        CHECK(out.find("class Pt:") != std::string::npos);
        CHECK(out.find("dist(self) -> Float") != std::string::npos);
        CHECK(out.find("_init_(self, x: Integer, y: Integer)") != std::string::npos);
        CHECK(out.find("_hidden") == std::string::npos);  // private attr not shown
    }

    // --- inspect: function signature ---
    {
        KiritoVM vm;
        std::string out = run(vm, "var f = Function(a : Integer, b = 1) -> String:\n    return \"x\"\ninspect(f)");
        CHECK(out.find("a: Integer") != std::string::npos);
        CHECK(out.find("b = ...") != std::string::npos);
        CHECK(out.find("-> String") != std::string::npos);
    }

    // --- inspect: module lists members ---
    {
        KiritoVM vm;
        std::string out = run(vm, "inspect(import(\"math\"))");
        CHECK(out.find("module math:") != std::string::npos);
        CHECK(out.find("sqrt") != std::string::npos);
    }

    // --- inspect: native function with a declared signature (params, types, defaults, return) ---
    {
        KiritoVM vm;
        std::string out = run(vm, "inspect(round)");
        CHECK(out.find("round(x: Number") != std::string::npos);
        CHECK(out.find("ndigits = None") != std::string::npos);

        std::string io = run(vm, "inspect(import(\"io\").open)");
        CHECK(io.find("open(path: String") != std::string::npos);
        CHECK(io.find("mode: String = \"r\"") != std::string::npos);
        CHECK(io.find("-> File") != std::string::npos);

        // a module's signatured function shows its signature in the module listing too.
        std::string mod = run(vm, "inspect(import(\"io\"))");
        CHECK(mod.find("open(path: String") != std::string::npos);
    }

    // --- inspect: builtins show their parameter NAMES; truly variadic builtins show `...` ---
    {
        KiritoVM vm;
        // signatured builtins name their parameters
        CHECK(run(vm, "inspect(Integer)") == "Integer(x) -> Integer");
        CHECK(run(vm, "inspect(Float)") == "Float(x) -> Float");
        CHECK(run(vm, "inspect(String)") == "String(x) -> String");
        CHECK(run(vm, "inspect(Bool)") == "Bool(x) -> Bool");
        CHECK(run(vm, "inspect(List)") == "List(iterable = None) -> List");
        CHECK(run(vm, "inspect(len)").find("len(x) -> Integer") != std::string::npos);
        CHECK(run(vm, "inspect(sorted)").find("key = None") != std::string::npos);
        CHECK(run(vm, "inspect(sorted)").find("reverse: Bool = False") != std::string::npos);
        // variadic builtins (no fixed parameter list) advertise themselves with `...`
        CHECK(run(vm, "inspect(min)") == "min(...)  [native]");
        CHECK(run(vm, "inspect(max)") == "max(...)  [native]");
        CHECK(run(vm, "inspect(zip)") == "zip(...)  [native]");
        CHECK(run(vm, "inspect(range)") == "range(...)  [native]");
        // io.print / write etc. are keyword-aware variadic -> still `...`
        CHECK(run(vm, "inspect(import(\"io\").print)") == "print(...)  [native]");
    }

    return RUN_TESTS();
}
