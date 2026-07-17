// Worked example of embedding & extending Kirito from C++ via the Value API. This file is the
// source of truth for the "Embedding" and "Extending" docs: a C++-authored module (`stats`) and a
// C++-authored value type (`Vec2`, with attributes, methods, an overloaded operator, and a custom
// str), both driven from Kirito source and indistinguishable from built-ins to the evaluator.
#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// ---- a C++-authored module ----------------------------------------------------------------------
// Subclass NativeModule, register members in setup() via the ModuleBuilder. Signatured functions
// (a param list + return type) gain keyword arguments, defaults, and `inspect` for free.
struct StatsModule : NativeModule {
    std::string name() const override { return "stats"; }

    void setup(ModuleBuilder& m) override {
        // mean(xs: List) -> Float — iterate any iterable, read each element as a number.
        m.fn("mean", {{"xs", "List"}}, "Float", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "mean");
            double sum = 0;
            int64_t n = 0;
            for (Value x : args.at(0).items()) { sum += x.asFloat("mean element"); ++n; }
            if (n == 0) throw KiritoError("mean of empty list");
            return Value(vm, sum / static_cast<double>(n));
        });

        // clamp(x, lo, hi) -> Integer — three typed args; callable with keywords thanks to the sig.
        m.fn("clamp", {{"x", "Integer"}, {"lo", "Integer"}, {"hi", "Integer"}}, "Integer",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 Args args(vm, a, "clamp");
                 int64_t x = args.at(0).asInt("x"), lo = args.at(1).asInt("lo"), hi = args.at(2).asInt("hi");
                 return Value(vm, std::max(lo, std::min(x, hi)));
             });

        m.value("VERSION", Value(m.vm(), "1.0"));  // a plain constant member
    }
};

// ---- a C++-authored value type ------------------------------------------------------------------
// Subclass NativeClass<Derived> (it fills in kind/typeName/truthy/equals); override only the
// protocol slots you need. Here: a custom str, attribute + method access (getAttr), and operator+.
struct Vec2 : NativeClass<Vec2> {
    static constexpr const char* kTypeName = "Vec2";
    double x, y;
    Vec2(double px, double py) : x(px), y(py) {}

    static std::string numstr(double d) {  // tidy: integral values print without a trailing ".0"
        return d == static_cast<int64_t>(d) ? std::to_string(static_cast<int64_t>(d)) : std::to_string(d);
    }
    std::string str(StringifyCtx&) const override { return "Vec2(" + numstr(x) + ", " + numstr(y) + ")"; }

    // Attribute reads (v.x, v.y) return values directly; method names return a NativeFunction with
    // `self` bound, so `v.length()` / `v.dot(o)` arrive with the receiver already in hand.
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        if (name == "x") return Value(vm, x);
        if (name == "y") return Value(vm, y);
        // makeMethod wraps a positional impl so the method ALSO accepts keyword arguments (here
        // `v.dot(other = ...)`); the named slots are declared as its `params`.
        if (name == "length")
            return makeMethod(vm, "length", {},
                [self](KiritoVM& mv, std::span<const Handle>) -> Handle {
                    auto& v = static_cast<Vec2&>(mv.arena().deref(self));
                    return Value(mv, std::sqrt(v.x * v.x + v.y * v.y));
                }, std::vector<Handle>{self});
        if (name == "dot")
            return makeMethod(vm, "dot", {"other"},
                [self](KiritoVM& mv, std::span<const Handle> a) -> Handle {
                    Args args(mv, a, "dot");
                    auto& v = static_cast<Vec2&>(mv.arena().deref(self));
                    auto* o = dynamic_cast<const Vec2*>(&mv.arena().deref(args.at(0)));
                    if (!o) throw KiritoError("dot expects a Vec2");
                    return Value(mv, v.x * o->x + v.y * o->y);
                }, std::vector<Handle>{self});
        return Object::getAttr(vm, self, name);  // anything else -> a clear "no attribute" error
    }

    // Operator overloading: Vec2 + Vec2 -> Vec2. Fall back to the base for anything unsupported.
    Handle binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) override {
        if (op == BinOp::Add)
            if (auto* o = dynamic_cast<const Vec2*>(&vm.arena().deref(rhs)))
                return vm.alloc(std::make_unique<Vec2>(x + o->x, y + o->y));
        return Object::binary(vm, op, self, rhs);
    }
};

int main() {
    KiritoVM vm;

    // register the module (one call) ...
    vm.install<StatsModule>();
    // ... and a constructor so `Vec2(x, y)` works from Kirito (signatured -> keyword args + inspect).
    vm.registerGlobal("Vec2", vm.alloc(std::make_unique<NativeFunction>(
        "Vec2", std::vector<NativeParam>{{"x", "Number"}, {"y", "Number"}}, "Vec2",
        [](KiritoVM& mv, std::span<const Handle> a) -> Handle {
            Args args(mv, a, "Vec2");
            return mv.alloc(std::make_unique<Vec2>(args.at(0).asFloat("x"), args.at(1).asFloat("y")));
        })));

    // ---- drive the C++-registered module and type from Kirito ----
    CHECK(vm.stringify(vm.runSource("import(\"stats\").mean([2, 4, 6, 8])")) == "5.0");
    CHECK(vm.stringify(vm.runSource("import(\"stats\").clamp(12, lo=0, hi=9)")) == "9");  // keyword args
    CHECK(vm.stringify(vm.runSource("import(\"stats\").VERSION")) == "1.0");

    CHECK(vm.stringify(vm.runSource("Vec2(3, 4).x")) == "3.0");
    CHECK(vm.stringify(vm.runSource("Vec2(3, 4).length()")) == "5.0");
    CHECK(vm.stringify(vm.runSource("Vec2(1, 2).dot(Vec2(3, 4))")) == "11.0");
    CHECK(vm.stringify(vm.runSource("Vec2(1, 2).dot(other = Vec2(3, 4))")) == "11.0");  // method keyword arg (makeMethod)
    CHECK(vm.stringify(vm.runSource("var s = Vec2(1, 2) + Vec2(3, 4)\nString(s)")) == "Vec2(4, 6)");
    CHECK(vm.stringify(vm.runSource("Vec2(x=3, y=4).length()")) == "5.0");  // constructor keyword args

    // bad arguments throw clean, catchable errors (never a crash)
    CHECK_THROWS(vm.runSource("import(\"stats\").mean(5)"));        // not a list
    CHECK_THROWS(vm.runSource("Vec2(1, 2).dot(42)"));              // dot wants a Vec2
    CHECK_THROWS(vm.runSource("Vec2(1, 2).nope"));                 // unknown attribute

    // the embedder reads a computed value back out
    Handle r = vm.runSource("6 * 7");
    CHECK(vm.arena().deref(r).kind() == ValueKind::Integer);
    CHECK(static_cast<const IntVal&>(vm.arena().deref(r)).value() == 42);

    // a second VM is fully independent: it never saw the 'stats' module or the Vec2 global
    KiritoVM vm2;
    CHECK_THROWS(vm2.runSource("import(\"stats\")"));
    CHECK_THROWS(vm2.runSource("Vec2(1, 2)"));

    return RUN_TESTS();
}
