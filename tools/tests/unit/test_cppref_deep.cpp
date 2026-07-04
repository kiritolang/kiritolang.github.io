// test_cppref_deep.cpp — adversarial/edge coverage for the C++ facade (value.hpp), filling audited
// gaps: the five never-called tryX() optionals, the unsigned/float primitive ctor overloads, the
// raw-Handle-key Dict/List overloads, Dict::tryGet(Value), .compare with a non-default rel_tol only,
// Value::at() on a String (negative + out of range), multi-byte String concat, the Bytes
// string_view ctor, and Value::equals on a cyclic structure (must terminate, not hang).
#include <optional>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    KiritoVM vm;

    // ---- tryX() optionals: has_value on the right kind, nullopt on the wrong kind ----
    CHECK(Value(vm, 1.5).tryFloatV().has_value());
    CHECK(!Value(vm, 1).tryFloatV().has_value());
    CHECK(Value(vm, "s").tryString().has_value());
    CHECK(!Value(vm, 1).tryString().has_value());
    CHECK(List(vm, {1, 2}).tryList().has_value());
    CHECK(!Value(vm, 1).tryList().has_value());
    CHECK(Dict(vm, {{"a", 1}}).tryDict().has_value());
    CHECK(!Value(vm, 1).tryDict().has_value());
    CHECK(Set(vm, {1, 2}).trySet().has_value());
    CHECK(!Value(vm, 1).trySet().has_value());

    // ---- unsigned / float primitive ctor overloads (adopt paths) ----
    CHECK(Value(vm, 5u).asInt() == 5);
    CHECK(Value(vm, 5ull).asInt() == 5);
    CHECK(Float(vm, 1.5f).value() == 1.5);
    CHECK(Integer(vm, 7u).value() == 7);
    CHECK(Integer(vm, 7ul).value() == 7);
    CHECK(Integer(vm, 7ull).value() == 7);

    // ---- raw-Handle-key Dict overloads + List::push(Handle) + Dict::tryGet(Value) ----
    {
        Dict d(vm);
        d.set(vm.makeInt(7), vm.makeString("x"));          // set(Handle, Handle)
        d.set(vm.makeInt(8), Value(vm, "y"));              // set(Handle, const Value&)
        CHECK(d.at(Value(vm, 7)).asStringRef() == "x");
        CHECK(d.at(Value(vm, 8)).asStringRef() == "y");
        CHECK(d.tryGet(Value(vm, 7)).has_value());
        CHECK(!d.tryGet(Value(vm, 99)).has_value());

        List xs(vm);
        xs.push(vm.makeInt(5));                             // push(Handle)
        xs.push(vm.makeInt(6));
        CHECK(xs.size() == 2 && xs[0].asInt() == 5 && xs[1].asInt() == 6);
    }

    // ---- .compare with a non-default rel_tol ONLY (abs_tol defaulted) ----
    CHECK(Integer(vm, 100).compare(Value(vm, 100.00001), 1e-3));
    CHECK(!Integer(vm, 100).compare(Value(vm, 200), 1e-3));
    CHECK(Float(vm, 1.0).compare(Value(vm, 1.0000001), 1e-3));
    CHECK(!Float(vm, 1.0).compare(Value(vm, 1.1), 1e-3));

    // ---- Value::at() on a String: negative index + out of range ----
    {
        Value s(vm, "abc");
        CHECK(s.at(0).asStringRef() == "a");
        CHECK(s.at(-1).asStringRef() == "c");
        CHECK_THROWS(s.at(99));
    }

    // ---- multi-byte String concat via String::operator+ ----
    {
        String a(vm, "café"), b(vm, "θ");
        String c = a + b;
        CHECK(c.size() == 5);                              // 4 code points + 1
        CHECK(c.utf8() == "caféθ");
        CHECK(c[4] == "θ");                                // re-index the concatenated result
    }

    // ---- Bytes(vm, std::string_view) ctor (distinct from the std::string overload) ----
    {
        Bytes b(vm, std::string_view("xyz"));
        CHECK(b.size() == 3);
        CHECK(b[0] == 'x' && b[2] == 'z');
    }

    // ---- Value::equals on a cyclic structure must TERMINATE (return or throw the depth guard) ----
    {
        Handle h = vm.runSource("var a = []\na.append(a)\na");
        Value av(vm, h);
        bool terminated = false;
        try { (void)av.equals(av); terminated = true; }
        catch (...) { terminated = true; }                 // depth-cap throw is fine; a hang is not
        CHECK(terminated);
    }

    // NOTE: the native-authoring internals (signatured NativeFunction accessors params()/returnType()/
    // acceptsKwargs(), NativeFunction::callKw with a NamedArg span, ModuleBuilder::alias error path,
    // registerModule factory) need native.hpp-precise construction and are covered separately by the
    // r4/r5/r7/r8 embed suites; this facade suite deliberately does not re-create them.

    return RUN_TESTS();
}
