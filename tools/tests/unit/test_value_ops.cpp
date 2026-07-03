// Exhaustive coverage of the ergonomic C++ facade's OPERATOR + SCALAR surface (value.hpp): every
// arithmetic/comparison operator (cross-checked byte-for-byte against the equivalent Kirito source so
// the "C++ a+b == Kirito a+b" guarantee is actually tested), integer wraparound, throw-on-zero-divisor,
// unordered-comparison throws, `contains`/`hash`/`equals`, and the Bool/Integer/Float/String/Bytes
// typed wrappers with their `.compare` tolerance, code-point indexing, and every type-mismatch error
// path. Adversarial: wrong-kind reads, out-of-range indices, uninitialised Value.
#include <climits>
#include <cstdint>
#include <optional>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    KiritoVM vm;

    // ===== operators are byte-identical to the equivalent Kirito expression =====
    auto same = [&](const char* expr, const Value& got) {
        return vm.stringify(got) == vm.stringify(vm.runSource(expr));
    };
    CHECK(same("2 + 3", Value(vm, 2) + Value(vm, 3)));
    CHECK(same("10 - 4", Value(vm, 10) - Value(vm, 4)));
    CHECK(same("6 * 7", Value(vm, 6) * Value(vm, 7)));
    CHECK(same("1 / 2", Value(vm, 1) / Value(vm, 2)));                 // true division -> Float
    CHECK(same("7 // 2", Value(vm, 7).floordiv(Value(vm, 2))));
    CHECK(same("7 % 3", Value(vm, 7) % Value(vm, 3)));
    CHECK(same("2 ** 10", Value(vm, 2).pow(Value(vm, 10))));
    CHECK(same("-5", -Value(vm, 5)));
    CHECK(same("1.5 + 2.5", Value(vm, 1.5) + Value(vm, 2.5)));
    CHECK(same("1.0 / 3.0", Value(vm, 1.0) / Value(vm, 3.0)));
    CHECK(same("\"a\" + \"b\"", Value(vm, "a") + Value(vm, "b")));
    CHECK(same("[1, 2] + [3]", List(vm, {1, 2}) + List(vm, {3})));
    CHECK(same("\"ab\" * 3", Value(vm, "ab") * Value(vm, 3)));

    // integer wraparound (two's-complement, well-defined) matches Kirito's
    CHECK(vm.stringify(Value(vm, static_cast<long long>(INT64_MAX)) + Value(vm, 1)) ==
          vm.stringify(vm.runSource("9223372036854775807 + 1")));

    // zero-divisor throws, on every division-flavoured operator
    CHECK_THROWS(Value(vm, 1) / Value(vm, 0));
    CHECK_THROWS(Value(vm, 1) % Value(vm, 0));
    CHECK_THROWS(Value(vm, 1).floordiv(Value(vm, 0)));

    // ===== comparisons =====
    CHECK(Value(vm, 2) < Value(vm, 3));
    CHECK(Value(vm, 3) > Value(vm, 2));
    CHECK(Value(vm, 2) <= Value(vm, 2));
    CHECK(Value(vm, 2) >= Value(vm, 2));
    CHECK(Value(vm, 2) == Value(vm, 2));
    CHECK(Value(vm, 2) != Value(vm, 3));
    CHECK(Value(vm, "a") < Value(vm, "b"));                            // lexicographic
    // `==`/`!=` never throw on a type mismatch — just report inequality
    CHECK(!(Value(vm, 1) == Value(vm, "a")));
    CHECK(Value(vm, 1) != Value(vm, "a"));
    // exact IEEE-754 equality: 0.1 + 0.2 != 0.3
    CHECK(vm.stringify(vm.runSource("0.1 + 0.2 == 0.3")) == "False");
    CHECK(!(Value(vm, 0.1) + Value(vm, 0.2) == Value(vm, 0.3)));
    // ordering an unordered pair throws (protocol has no int<string)
    CHECK_THROWS(Value(vm, 1) < Value(vm, "a"));

    // ===== truthiness =====
    CHECK(Value(vm, 5).truthy());
    CHECK(!Value(vm, 0).truthy());
    CHECK(!Value(vm, "").truthy());
    CHECK(!Value::None(vm).truthy());
    CHECK(static_cast<bool>(Value(vm, 5)));
    CHECK(!Value(vm, 0));                                              // operator!

    // ===== contains (`in`) =====
    {
        Value xs(vm, List(vm, {1, 2, 3}).handle());
        CHECK(xs.contains(Value(vm, 2)));
        CHECK(!xs.contains(Value(vm, 99)));
        Value txt(vm, "hello");
        CHECK(txt.contains(Value(vm, "ell")));
        CHECK(!txt.contains(Value(vm, "xyz")));
    }

    // ===== equals + hash =====
    CHECK(Value(vm, 1).equals(Value(vm, 1)));
    CHECK(!Value(vm, 1).equals(Value(vm, 2)));
    CHECK(List(vm, {1, 2}).equals(List(vm, {1, 2})));
    CHECK(Value(vm, 7).hash() == Value(vm, 7).hash());                 // stable
    CHECK(Value(vm, "k").hash() == Value(vm, "k").hash());
    CHECK_THROWS(List(vm, {1, 2}).hash());                            // list is unhashable
    CHECK_THROWS(Set(vm, {1, 2}).hash());
    CHECK_THROWS(Dict(vm, {{"a", 1}}).hash());

    // ===== Bool wrapper =====
    {
        Bool bt(vm, true), bf(vm, false);
        CHECK(bt.value() == true && bf.value() == false);
        CHECK(static_cast<bool>(bt) && !static_cast<bool>(bf));
        CHECK(Value(vm, true).asBoolV().value());
        CHECK_THROWS(Value(vm, 1).asBoolV());                         // Integer is not a Bool
        CHECK(Value(vm, true).tryBoolV().has_value());
        CHECK(!Value(vm, 1).tryBoolV().has_value());
    }

    // ===== Integer wrapper (+ tolerance .compare in every overload) =====
    {
        Integer i5(vm, 5);
        CHECK(i5.value() == 5);
        CHECK(static_cast<int64_t>(i5) == 5);
        CHECK(i5.compare(5.0));                                        // double overload
        CHECK(i5.compare(static_cast<int64_t>(5)));                    // int64 overload
        CHECK(i5.compare(Value(vm, 5.0)));                            // Value overload
        CHECK(i5.compare(5.0 + 1e-12));                               // within default rel_tol
        CHECK(!i5.compare(6.0));
        CHECK(i5.compare(6.0, 1.0, 2.0));                            // abs_tol swallows the gap
        CHECK(Value(vm, 5).asInteger().value() == 5);
        CHECK_THROWS(Value(vm, "x").asInteger());
        CHECK_THROWS(Value(vm, 1.0).asInteger());                    // Float is not Integer
        CHECK(Value(vm, 5).tryInteger().has_value());
        CHECK(!Value(vm, 5.0).tryInteger().has_value());
    }

    // ===== Float wrapper =====
    {
        Float f(vm, 3.5);
        CHECK(f.value() == 3.5);
        CHECK(static_cast<double>(f) == 3.5);
        CHECK(f.compare(3.5));
        CHECK(f.compare(3.5 + 1e-12));
        CHECK(!f.compare(4.0));
        CHECK(f.compare(3.6, 0.0, 0.2));                             // abs_tol
        CHECK(Value(vm, 2.0).asFloatV().value() == 2.0);
        CHECK_THROWS(Value(vm, 1).asFloatV());                       // Integer is not a Float here
        // asFloat() (the double reader) DOES accept an Integer — distinct from asFloatV()
        CHECK(Value(vm, 7).asFloat() == 7.0);
    }

    // ===== String wrapper: code points, slicing, search, concat, literal-equality =====
    {
        String s(vm, "héllo");                                        // é is one code point, 2 bytes
        CHECK(s.size() == 5);
        CHECK(s.utf8().size() == 6);
        CHECK(s.value() == "héllo");
        CHECK(!s.empty());
        CHECK(s[0] == "h");
        CHECK(s[1] == "é");                                           // multi-byte code point
        CHECK(s[-1] == "o");
        CHECK_THROWS(s[5]);
        CHECK_THROWS(s[-6]);
        CHECK(s.contains("ll"));
        CHECK(!s.contains("zz"));
        CHECK(s.startsWith("hé"));
        CHECK(s.endsWith("lo"));
        String cat = String(vm, "foo") + String(vm, "bar");
        CHECK(cat == "foobar");
        CHECK(cat != "nope");
        std::string_view sv = s;                                      // implicit conversion
        CHECK(sv == "héllo");
        CHECK(Value(vm, "abc").asString().size() == 3);
        CHECK_THROWS(Value(vm, 1).asString());
        String empty(vm, "");
        CHECK(empty.empty() && empty.size() == 0);
        CHECK_THROWS(empty[0]);
    }

    // ===== Bytes wrapper: byte-at-index, negative index, out of range =====
    {
        Bytes b(vm, std::string("\x00\xff\x10", 3));                  // explicit length keeps the NUL
        CHECK(b.isBytes());
        CHECK(b.size() == 3 && !b.empty());
        CHECK(b[0] == 0);
        CHECK(b[1] == 255);
        CHECK(b[2] == 16);
        CHECK(b[-1] == 16);
        CHECK_THROWS(b[3]);
        CHECK_THROWS(b[-4]);
        CHECK(b.data().size() == 3);
        CHECK(Value(vm, b.handle()).asBytes().size() == 3);
        CHECK_THROWS(Value(vm, 1).asBytes());
        CHECK(b.tryBytes().has_value());                             // NB: Bytes IS-A Value
        CHECK(!Value(vm, 1).tryBytes().has_value());
    }

    // ===== Args::require wording + missing-arg diagnostics, via a registered native =====
    vm.registerGlobal("needtwo", vm.arena().alloc(std::make_unique<NativeFunction>(
        "needtwo", [](KiritoVM& kv, std::span<const Handle> raw) -> Handle {
            Args a(kv, raw, "needtwo");
            a.require(2);
            return kv.makeInt(a[0].asInt("x") + a[1].asInt("y"));
        })));
    CHECK(vm.stringify(vm.runSource("needtwo(3, 4)")) == "7");
    {
        bool w = false;
        try { vm.runSource("needtwo(1)"); } catch (const KiritoError& e) {
            w = std::string(e.what()).find("expected at least 2") != std::string::npos;
        }
        CHECK(w);
    }

    // ===== call() forwarding a mixed initializer list =====
    {
        Value lenFn(vm, vm.runSource("len"));
        CHECK(lenFn.call({List(vm, {1, 2, 3, 4})}).asInt() == 4);
        Value absFn(vm, vm.runSource("abs"));
        CHECK(absFn.call({-9}).asInt() == 9);
    }

    // ===== getAttr / setAttr =====
    {
        Value mathmod(vm, vm.runSource("import(\"math\")"));
        double pi = mathmod.getAttr("pi").asFloat();
        CHECK(pi > 3.14159 && pi < 3.14160);
        Value inst(vm, vm.runSource("class Pt:\n    var x = 1\nPt()"));
        CHECK(inst.getAttr("x").asInt() == 1);
        inst.setAttr("x", Value(vm, 42));
        CHECK(inst.getAttr("x").asInt() == 42);
    }

    // ===== uninitialised Value: every accessor throws, none crash =====
    {
        Value empty;
        CHECK(!empty.bound());
        CHECK_THROWS(empty.typeName());
        CHECK_THROWS(empty.asInt());
        CHECK_THROWS(empty.truthy());
        CHECK_THROWS(empty.len());
    }

    return RUN_TESTS();
}
