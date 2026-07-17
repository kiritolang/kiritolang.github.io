// Coverage for the C++ extension/embedding registration paths not exercised elsewhere:
// ModuleBuilder::kwfn (keyword-aware variadic native), ModuleBuilder::value/alias, the signatured fn
// + argInt/argString helpers and their throws, makeMethod keyword binding + its error paths, a bare
// NativeClass's CRTP defaults, registerSourceModule (third-party frozen module), registerClass/
// findClass, and makeBool interning.
#include <span>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string ev(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }

// A bare NativeClass with no overrides -> exercises the CRTP defaults (typeName/truthy/str/equals).
struct Blank : NativeClass<Blank> {
    static constexpr const char* kTypeName = "Blank";
};

// A NativeClass whose method is bound with makeMethod -> exercises the keyword-binding machinery.
struct Widget : NativeClass<Widget> {
    static constexpr const char* kTypeName = "Widget";
    int64_t base;
    explicit Widget(int64_t b) : base(b) {}
    Handle getAttr(KiritoVM& kv, Handle self, std::string_view name) override {
        if (name == "scale")
            return makeMethod(kv, "scale", {"factor", "bias"},
                [self](KiritoVM& mv, std::span<const Handle> a) -> Handle {
                    auto opt = [&](std::size_t i, int64_t d) -> int64_t {
                        if (i >= a.size()) return d;
                        Object& o = mv.arena().deref(a[i]);
                        return o.kind() == ValueKind::Integer ? static_cast<const IntVal&>(o).value() : d;
                    };
                    int64_t b = static_cast<Widget&>(mv.arena().deref(self)).base;
                    return mv.makeInt(b * opt(0, 1) + opt(1, 0));
                }, std::vector<Handle>{self});
        return Object::getAttr(kv, self, name);
    }
};

// A module exercising fn (signatured), kwfn (variadic + named), value, alias, argInt/argString.
struct DemoMod : NativeModule {
    std::string name() const override { return "demo"; }
    void setup(ModuleBuilder& m) override {
        m.fn("twice", {{"x", "Integer"}}, "Integer", [](KiritoVM& kv, std::span<const Handle> a) -> Handle {
            return kv.makeInt(argInt(kv, a[0], "x") * 2);
        });
        m.fn("shout", {{"s", "String"}}, "String", [](KiritoVM& kv, std::span<const Handle> a) -> Handle {
            return kv.makeString(argString(kv, a[0], "s") + "!");
        });
        m.kwfn("collect", [](KiritoVM& kv, std::span<const Handle> pos,
                             std::span<const NamedArg> named) -> Handle {
            int64_t sum = 0;
            for (Handle h : pos) sum += static_cast<const IntVal&>(kv.arena().deref(h)).value();
            for (const auto& na : named)
                if (na.name == "bonus") sum += static_cast<const IntVal&>(kv.arena().deref(na.value)).value();
            return kv.makeInt(sum);
        });
        m.value("answer", m.vm().makeInt(42));
        m.alias("twice2", "twice");
    }
};

int main() {
    KiritoVM vm;
    vm.install<DemoMod>();

    // ---- signatured fn accepts positional + keyword ----
    CHECK(ev(vm, "import(\"demo\").twice(5)") == "10");
    CHECK(ev(vm, "import(\"demo\").twice(x = 6)") == "12");
    // ---- argInt / argString throws ----
    CHECK_THROWS(vm.runSource("import(\"demo\").twice(\"nope\")"));
    CHECK(ev(vm, "import(\"demo\").shout(\"hi\")") == "hi!");
    CHECK_THROWS(vm.runSource("import(\"demo\").shout(5)"));

    // ---- kwfn: keyword-aware variadic native (positionals + a named option) ----
    CHECK(ev(vm, "import(\"demo\").collect(1, 2, 3)") == "6");
    CHECK(ev(vm, "import(\"demo\").collect(1, 2, bonus = 10)") == "13");

    // ---- module value + alias ----
    CHECK(ev(vm, "import(\"demo\").answer") == "42");
    CHECK(ev(vm, "import(\"demo\").twice2(4)") == "8");

    // ---- makeMethod keyword binding + the error paths ----
    vm.registerGlobal("Widget", vm.arena().alloc(std::make_unique<NativeFunction>(
        "Widget", [](KiritoVM& kv, std::span<const Handle> a) -> Handle {
            return kv.arena().alloc(std::make_unique<Widget>(
                static_cast<const IntVal&>(kv.arena().deref(a[0])).value()));
        })));
    CHECK(ev(vm, "Widget(10).scale(factor = 3, bias = 1)") == "31");
    CHECK(ev(vm, "Widget(10).scale(2)") == "20");                 // positional factor, bias defaults
    CHECK(ev(vm, "Widget(10).scale(bias = 5, factor = 2)") == "25");
    CHECK_THROWS(vm.runSource("Widget(10).scale(nope = 1)"));      // unexpected keyword
    CHECK_THROWS(vm.runSource("Widget(10).scale(2, factor = 3)")); // multiple values for 'factor'

    // ---- bare NativeClass CRTP defaults ----
    vm.registerGlobal("Blank", vm.arena().alloc(std::make_unique<NativeFunction>(
        "Blank", [](KiritoVM& kv, std::span<const Handle>) -> Handle {
            return kv.arena().alloc(std::make_unique<Blank>());
        })));
    CHECK(ev(vm, "String(Blank())") == "<Blank>");
    CHECK(ev(vm, "Bool(Blank())") == "True");                      // default truthy
    CHECK(ev(vm, "type(Blank())") == "Blank");
    CHECK(ev(vm, "var b = Blank()\nb == b") == "True");            // identity equals (same object)
    CHECK(ev(vm, "Blank() == Blank()") == "False");               // distinct objects

    // ---- registerSourceModule: a third-party frozen Kirito module ----
    vm.registerSourceModule("frozen", "var x = 7\nvar dbl = Function(n): return n * 2\n");
    CHECK(ev(vm, "import(\"frozen\").x") == "7");
    CHECK(ev(vm, "import(\"frozen\").dbl(21)") == "42");

    // ---- registerClass / findClass ----
    vm.runSource("class Foo:\n    var _init_ = Function(self, v):\n        self.v = v\n");
    CHECK(vm.findClass("Foo") != nullptr);
    CHECK(vm.findClass("Nope") == nullptr);

    // ---- makeBool interning (True/False are singletons) ----
    CHECK(ev(vm, "id(True) == id(True)") == "True");
    CHECK(ev(vm, "id(True) == id(False)") == "False");

    return RUN_TESTS();
}
