#include <iostream>
#include <sstream>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

// Temporarily redirect std::cout / std::cin for the io tests.
struct CoutCapture {
    std::ostringstream oss;
    std::streambuf* old = std::cout.rdbuf(oss.rdbuf());
    ~CoutCapture() { std::cout.rdbuf(old); }
};
struct CinFeed {
    std::istringstream iss;
    std::streambuf* old;
    explicit CinFeed(std::string s) : iss(std::move(s)), old(std::cin.rdbuf(iss.rdbuf())) {}
    ~CinFeed() { std::cin.rdbuf(old); }
};

// A C++-authored module, registered exactly as a third party would.
struct MathMod : NativeModule {
    std::string name() const override { return "math2"; }
    void setup(ModuleBuilder& m) override {
        m.fn("square", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            int64_t n = static_cast<const IntVal&>(vm.arena().deref(a[0])).value();
            return vm.makeInt(n * n);
        });
    }
};

// A C++-authored object type, exposed via a registered constructor.
struct Point : NativeClass<Point> {
    static constexpr const char* kTypeName = "Point";
    int64_t x, y;
    Point(int64_t px, int64_t py) : x(px), y(py) {}
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        if (name == "x") return vm.makeInt(x);
        if (name == "y") return vm.makeInt(y);
        return Object::getAttr(vm, self, name);
    }
};

int main() {
    // import returns a module; modules are per-VM singletons (same handle each time)
    {
        KiritoVM vm;
        Handle m1 = vm.runSource("import(\"io\")");
        Handle m2 = vm.runSource("import(\"io\")");
        CHECK(m1 == m2);
        CHECK(vm.arena().deref(m1).kind() == ValueKind::Module);
        CHECK_THROWS(vm.runSource("import(\"nope\")"));
    }

    // io.print writes to stdout
    {
        KiritoVM vm;
        CoutCapture cap;
        vm.runSource("var io = import(\"io\")\nio.print(\"hi\")\n");
        CHECK(cap.oss.str() == "hi\n");
    }

    // type conversions
    {
        KiritoVM vm;
        CHECK(evalStr(vm, "Integer(\"42\")") == "42");
        CHECK(evalStr(vm, "Integer(3.9)") == "3");
        CHECK(evalStr(vm, "Float(\"1.5\")") == "1.5");
        CHECK(evalStr(vm, "String(42)") == "42");
        CHECK(evalStr(vm, "String(1 == 1)") == "True");
        CHECK(evalStr(vm, "Bool(0)") == "False");
        CHECK_THROWS(vm.runSource("Integer(\"abc\")"));
    }

    // the north-star program: input 5 -> "525"
    {
        CinFeed in("5\n");
        KiritoVM vm;
        CoutCapture cap;
        vm.runSource(
            "var io = import(\"io\")\n"
            "var main = Function():\n"
            "    var n = Integer(io.input())\n"
            "    io.print(String(n) + String(n * n))\n"
            "main()\n");
        CHECK(cap.oss.str() == "525\n");
    }

    // extension: a C++ NativeModule, driven from Kirito
    {
        KiritoVM vm;
        vm.install<MathMod>();
        CHECK(evalStr(vm, "import(\"math2\").square(6)") == "36");
    }

    // extension: a C++ NativeClass with a registered constructor, driven from Kirito
    {
        KiritoVM vm;
        vm.registerGlobal("Point", vm.arena().alloc(std::make_unique<NativeFunction>(
            "Point", [](KiritoVM& kv, std::span<const Handle> a) -> Handle {
                int64_t x = static_cast<const IntVal&>(kv.arena().deref(a[0])).value();
                int64_t y = static_cast<const IntVal&>(kv.arena().deref(a[1])).value();
                return kv.arena().alloc(std::make_unique<Point>(x, y));
            })));
        CHECK(evalStr(vm, "var p = Point(3, 4)\np.x + p.y") == "7");
    }

    return RUN_TESTS();
}
