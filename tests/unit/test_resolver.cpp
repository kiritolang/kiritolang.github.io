// Deep tests for the compile-time, scope-aware name resolver (resolver.hpp). Resolution runs before
// execution, so an undefined name is a COMPILE-time error (thrown from runSource before any code runs)
// — these tests drive it through the public VM API. errOf() returns the (compile or run) error text or
// "" on success; programs are chosen so the ONLY possible error is name resolution.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string errOf(const std::string& src) {
    KiritoVM vm;
    try { vm.runSource(src); return ""; }
    catch (const std::exception& e) { return e.what(); }
}
static bool nameError(const std::string& src) {
    return errOf(src).find("is not defined") != std::string::npos;
}
static bool ok(const std::string& src) { return errOf(src).empty(); }

int main() {
    // === undefined names are compile-time errors ===
    CHECK(nameError("undefined_xyz"));
    CHECK(nameError("var y = undefined_xyz"));
    CHECK(nameError("x = 5"));                                  // bare = to a never-declared name
    CHECK(nameError("io.print(missing_arg)"));                 // inside a call (io is defined via... no)
    CHECK(nameError("var io = import(\"io\")\nio.print(missing_arg)"));
    // caught even where the reference never executes:
    CHECK(nameError("var f = Function(): return never_defined\n5"));   // uncalled function body
    CHECK(nameError("if False:\n    dead_ref\n1"));                    // untaken branch
    CHECK(nameError("var r = 1 if True else never2"));                 // untaken conditional arm
    CHECK(nameError("False and never3"));                             // short-circuited operand
    CHECK(nameError("var g = Function():\n    var h = Function(): return deeply_missing\n    return h\n0"));  // nested fn

    // === names that MUST resolve (no false positives) ===
    CHECK(ok("var x = 5\nx"));
    CHECK(ok("var x = 5\nx = 6\nx"));                           // bare = to a declared local
    CHECK(ok("var fac = Function(n):\n    if n <= 1:\n        return 1\n    return n * fac(n-1)\nfac(5)"));  // recursion
    CHECK(ok("var ev = Function(n):\n    return True if n==0 else od(n-1)\n"
             "var od = Function(n):\n    return False if n==0 else ev(n-1)\nev(4)"));  // mutual recursion
    CHECK(ok("var main = Function():\n    return helper()\nvar helper = Function(): return 1\nmain()"));  // forward ref
    CHECK(ok("var use = Function(): return later\nvar later = 7\nuse()"));  // forward ref to module global
    CHECK(ok("len([1,2,3])"));                                  // builtin
    CHECK(ok("List(range(3))"));                                // type constructor
    CHECK(ok("import(\"math\").pi"));                           // builtin import
    CHECK(ok("var make = Function(b):\n    return Function(x): return x + b\nmake(10)(5)"));  // closure capture
    CHECK(ok("for i in range(3):\n    discard i"));             // for-loop var
    CHECK(ok("var d = {\"a\":1}\nfor k, v in d.items():\n    discard k\n    discard v"));  // unpacking for-vars

    // forward reference WITHIN a scope (membership, not textual order)
    CHECK(ok("var f = Function():\n    var a = b\n    var b = 5\n    return a\n0"));  // a=b resolves (b declared below)

    // === class scopes ===
    CHECK(ok("class P:\n    var _init_ = Function(self, x):\n        self.x = x\n    var get = Function(self): return self.x\nP(3).get()"));
    CHECK(ok("class A:\n    var who = Function(self): return \"A\"\n"
             "class B(A):\n    var who = Function(self): return self._super_().who()\nB().who()"));  // _super_
    CHECK(ok("class K:\n    var n = 7\n    var get = Function(self): return n\nK()"));  // class-body name by bare ref
    CHECK(nameError("class Q:\n    var m = Function(self): return undefined_in_method\nQ"));  // typo in method

    // === catch / with bind names in their scope ===
    CHECK(ok("try:\n    throw \"x\"\ncatch as e:\n    e"));
    CHECK(ok("class CM:\n    var _enter_ = Function(self): return 1\n    var _exit_ = Function(self): return None\n"
             "with CM() as c:\n    discard c"));

    // === arglist / argmain are always in scope ===
    CHECK(ok("len(arglist)"));
    CHECK(ok("argmain"));

    // === param defaults are checked in the enclosing scope ===
    CHECK(ok("var base = 10\nvar f = Function(x = base): return x\nf()"));
    CHECK(nameError("var f = Function(x = missing_default): return x\n0"));

    // === a member access (obj.name) does NOT resolve `name` as a scope name ===
    CHECK(ok("var s = \"hi\"\ns.upper()"));                     // upper is a member, not a name
    CHECK(ok("var d = {}\nd.keys()"));

    // === conditionally-declared name: in scope (membership), so a try/catch can still catch the
    // runtime "unbound" case — i.e. NOT a compile error ===
    {
        // `maybe` is declared (var in the untaken branch), so referencing it is NOT a compile error;
        // at run time it is unbound and throws, which the try/catch handles.
        std::string src = "var got = \"none\"\n"
                          "if False:\n    var maybe = 1\n"
                          "try:\n    got = maybe\ncatch as e:\n    got = \"caught\"\n";
        CHECK(ok(src));  // compiles (maybe is in scope by membership)
    }

    // === REPL: bindings accumulate across lines; later lines resolve earlier names ===
    {
        KiritoVM vm;
        bool threw = false;
        try {
            vm.runRepl("var counter = 41");
            vm.runRepl("counter = counter + 1");   // references counter from the prior line
            vm.runRepl("counter");
        } catch (...) { threw = true; }
        CHECK(!threw);
    }

    // === embedder-registered globals resolve ===
    {
        KiritoVM vm;
        vm.registerGlobal("injected", vm.makeInt(42));
        bool threw = false;
        try { vm.runSource("injected + 1"); } catch (...) { threw = true; }
        CHECK(!threw);
    }

    // === the error carries a location (line/col) ===
    {
        KiritoVM vm;
        try { vm.runSource("var a = 1\nvar b = bad_name\n"); CHECK(false); }
        catch (const KiritoError& e) {
            CHECK(std::string(e.what()).find("bad_name") != std::string::npos);
            CHECK(e.span.line == 2);
        }
    }

    return RUN_TESTS();
}
