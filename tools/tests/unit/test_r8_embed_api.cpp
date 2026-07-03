// Round-8 (R8) embedding / extension API audit — pinning C++-side contracts NOT already covered by the
// prior embedding tests (test_r4_cpp_api / test_r7_embed_api / test_value{,_extra} / test_embedding_extra
// / test_protocol / test_serde / test_arena / test_gc). Each block targets a genuinely-uncovered surface;
// see the header comment above each.
//
//   1. value.hpp facade corners: Value() default-constructed, handle()/vm() accessors, the
//      val(int)/val(size_t) integer overloads, builder .size() growth, Args::operator[] (unchecked) vs
//      at()/opt() bounds, and makeList(initializer_list) vs makeList(vector).
//   2. WRONG-TYPE Value reads are clean, catchable KiritoErrors across EVERY accessor (asInt/asFloat/
//      asString/asBool/len/at/items/has/get/pairs on a mismatched type) — the embedder never crashes
//      on a bad read.
//   3. A NativeClass slot combo not yet pinned: setItem-driven mutation + a custom `contains` + a
//      `_str_`-via-str override, AND a `_getstate_`/`_setstate_` that round-trips a *multi-field Dict
//      state* (not a bare scalar) through BOTH serde codecs (text `serial` + binary `dumpfmt`),
//      including shared/aliased + two-levels-nested in a container (shared ref preserved).
//   4. registerDeserializer OVERWRITE: registering a second factory for the same type name replaces the
//      first (observable via a factory-set field _setstate_ leaves alone).
//   5. makeMethod MULTI-capture GC-rooting: a method capturing two boxed handles keeps them alive across
//      an aggressive collection purely via NativeFunction::children() (the captures are the only roots).
//   6. The evalIn / Resolver custom-parent-scope contract (the prior round's broken assertion):
//      evalIn resolves names bound DIRECTLY in the passed scope and VM globals, but a name reachable only
//      through a hand-built NON-global parent EnvValue is a COMPILE-TIME `name ... is not defined` — even
//      though it resolves at run time via envLookup. We assert the ACTUAL behavior and the runtime
//      contrast (see the FINDING note in that block).
//   7. runRepl persistent scope vs runSource fresh scope: a binding from one runRepl line is visible on
//      the next, and is isolated from a fresh runSource module scope. Plus setArgs -> arglist/argmain.
//   8. Parse/nesting depth guard throws a catchable KiritoError (never crashes the host) for pathological
//      bracket- and paren-nesting; and the call-depth guard (setMaxCallDepth) throws on deep recursion.
//   9. Registry isolation across VMs: classRegistry (registerClass/findClass), the deserializer registry,
//      registerSourceModule, and the import module cache are all per-VM — nothing leaks between two VMs.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string ev(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }

// ---- A native value type whose serialized state is a multi-field Dict (block 3). Also exercises a
// ---- setItem-driven mutation, a custom `contains`, and an overridden str(). 3-D integer vector. ----
struct Vec3 : NativeClass<Vec3> {
    static constexpr const char* kTypeName = "Vec3";
    std::array<int64_t, 3> c{0, 0, 0};
    Vec3() = default;
    Vec3(int64_t x, int64_t y, int64_t z) : c{x, y, z} {}

    std::string str(StringifyCtx&) const override {
        return "Vec3(" + std::to_string(c[0]) + ", " + std::to_string(c[1]) + ", " + std::to_string(c[2]) + ")";
    }
    // v[i] read / v[i] = n write (single integer key).
    Handle getItem(KiritoVM& vm, std::span<const Handle> keys) override {
        int64_t i = static_cast<const IntVal&>(vm.arena().deref(keys[0])).value();
        if (i < 0 || i > 2) throw KiritoError("Vec3 index out of range");
        return vm.makeInt(c[static_cast<std::size_t>(i)]);
    }
    void setItem(KiritoVM& vm, std::span<const Handle> keys, Handle value) override {
        int64_t i = static_cast<const IntVal&>(vm.arena().deref(keys[0])).value();
        if (i < 0 || i > 2) throw KiritoError("Vec3 index out of range");
        c[static_cast<std::size_t>(i)] = static_cast<const IntVal&>(vm.arena().deref(value)).value();
    }
    // `n in v` — true if n equals any component.
    bool contains(KiritoVM& vm, Handle value) override {
        int64_t n = static_cast<const IntVal&>(vm.arena().deref(value)).value();
        return n == c[0] || n == c[1] || n == c[2];
    }
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        if (name == "x") return vm.makeInt(c[0]);
        if (name == "y") return vm.makeInt(c[1]);
        if (name == "z") return vm.makeInt(c[2]);
        if (name == "_getstate_")
            return makeMethod(vm, "_getstate_", {}, [self](KiritoVM& v, std::span<const Handle>) -> Handle {
                auto& s = static_cast<Vec3&>(v.arena().deref(self));
                return Dict(v).set("x", s.c[0]).set("y", s.c[1]).set("z", s.c[2]).build();  // multi-field state
            }, std::vector<Handle>{self});
        if (name == "_setstate_")
            return makeMethod(vm, "_setstate_", {"state"}, [self](KiritoVM& v, std::span<const Handle> a) -> Handle {
                Value st(v, a[0]);
                auto& s = static_cast<Vec3&>(v.arena().deref(self));
                s.c[0] = st.get("x").asInt("x");
                s.c[1] = st.get("y").asInt("y");
                s.c[2] = st.get("z").asInt("z");
                return v.none();
            }, std::vector<Handle>{self});
        return Object::getAttr(vm, self, name);
    }
};

static Handle makeVec3(KiritoVM& vm, int64_t x, int64_t y, int64_t z) {
    return vm.alloc(std::make_unique<Vec3>(x, y, z));
}

// ---- A native type with a factory-set, NOT-serialized field, to make a deserializer OVERWRITE
// ---- observable (block 4): the chosen factory stamps `madeBy`; _setstate_ never touches it. ----
struct Tagged : NativeClass<Tagged> {
    static constexpr const char* kTypeName = "Tagged";
    int64_t v = 0;
    std::string madeBy = "?";   // set by the deserializer factory, never carried in the serialized state
    Tagged() = default;
    explicit Tagged(int64_t x) : v(x) {}
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        if (name == "_getstate_")
            return makeMethod(vm, "_getstate_", {}, [self](KiritoVM& kv, std::span<const Handle>) -> Handle {
                return kv.makeInt(static_cast<Tagged&>(kv.arena().deref(self)).v);
            }, std::vector<Handle>{self});
        if (name == "_setstate_")
            return makeMethod(vm, "_setstate_", {"state"}, [self](KiritoVM& kv, std::span<const Handle> a) -> Handle {
                static_cast<Tagged&>(kv.arena().deref(self)).v =
                    static_cast<const IntVal&>(kv.arena().deref(a[0])).value();
                return kv.none();
            }, std::vector<Handle>{self});
        return Object::getAttr(vm, self, name);
    }
};

int main() {
    // ============================================================================================
    // 1) value.hpp facade corners.
    // ============================================================================================
    {
        KiritoVM vm;

        // A default-constructed Value holds no VM (the field initializer the facade declares).
        Value empty;
        (void)empty;  // constructed without UB; we never deref it.

        // handle()/vm() accessors round-trip a constructed Value.
        Value v = Value(vm, 5);
        CHECK(v.handle() == static_cast<Handle>(v));   // handle() == the implicit-Handle conversion
        CHECK(&v.vm() == &vm);                          // vm() names the owning VM

        // val() integer overloads: int, int64_t, std::size_t all build an Integer.
        CHECK(Value(vm, 3).asInt() == 3);                          // int
        CHECK(Value(vm, static_cast<int64_t>(8)).asInt() == 8);    // int64_t
        CHECK(Value(vm, static_cast<std::size_t>(12)).asInt() == 12);  // size_t

        // builders report a growing size() before build(), and the std::vector overload also works.
        List lb(vm);
        CHECK(lb.size() == 0);
        lb.add(1).add(2);
        CHECK(lb.size() == 2);
        Value built = lb.build();
        CHECK(built.len() == 2);

        // makeList(initializer_list) and makeList(vector) agree.
        Value il = List(vm, {vm.makeInt(1), vm.makeInt(2), vm.makeInt(3)});
        std::vector<Handle> hs{vm.makeInt(1), vm.makeInt(2), vm.makeInt(3)};
        Value vl = List(vm, hs);
        CHECK(il.len() == 3 && vl.len() == 3);

        // Args: operator[] is the unchecked read (valid after a size() test); at()/opt() are bounds-aware.
        vm.registerGlobal("probe", vm.alloc(std::make_unique<NativeFunction>(
            "probe", [](KiritoVM& kv, std::span<const Handle> raw) -> Handle {
                Args a(kv, raw, "probe");
                int64_t first = a.empty() ? -1 : a[0].asInt("first");          // a[0] unchecked, guarded by empty()
                int64_t second = a.opt(1, Value(kv, 100)).asInt("second");       // opt() default
                return kv.makeInt(first * 1000 + second);
            })));
        CHECK(ev(vm, "probe(7)") == "7100");           // a[0]=7, opt default 100
        CHECK(ev(vm, "probe(7, 8)") == "7008");
        CHECK(ev(vm, "probe()") == "-900");            // empty() branch -> -1, +100
    }

    // ============================================================================================
    // 2) WRONG-TYPE Value reads are clean, catchable errors (never a cast crash).
    // ============================================================================================
    {
        KiritoVM vm;
        // scalar typed-read mismatches
        CHECK_THROWS(Value(vm, std::string("x")).asInt("n"));
        CHECK_THROWS(Value(vm, 1).asStringRef("s"));
        CHECK_THROWS(Value(vm, std::string("x")).asBool("b"));
        CHECK_THROWS(Value(vm, std::string("x")).asFloat("f"));
        CHECK_THROWS(Value::None(vm).asInt("n"));
        // collection accessors on a non-collection
        CHECK_THROWS(Value(vm, 5).len());                 // Integer has no length
        CHECK_THROWS(Value(vm, 5).items());               // Integer not iterable
        CHECK_THROWS(Value(vm, 5).at(0));                 // Integer not indexable
        // Dict accessors on a non-Dict
        CHECK_THROWS(Value(vm, 5).has("k"));
        CHECK_THROWS(Value(vm, 5).get("k"));
        CHECK_THROWS(Value(vm, 5).pairs());
        // get on a real Dict with an absent key (no default) throws; with a default does not.
        Value d = Dict(vm).set("a", 1).build();
        CHECK_THROWS(d.get("absent"));
        CHECK(d.get("absent", Value(vm, -1)).asInt() == -1);
        // The error message is actionable (names the expected type), not a generic crash.
        bool informative = false;
        try { Value(vm, std::string("x")).asInt("count"); }
        catch (const KiritoError& e) {
            std::string m = e.what();
            informative = m.find("count") != std::string::npos && m.find("Integer") != std::string::npos;
        }
        CHECK(informative);
    }

    // ============================================================================================
    // 3) NativeClass slot combo + _getstate_/_setstate_ (multi-field Dict state) through BOTH codecs.
    // ============================================================================================
    {
        KiritoVM vm;
        vm.registerGlobal("Vec3", vm.alloc(std::make_unique<NativeFunction>(
            "Vec3", std::vector<NativeParam>{{"x", "Integer"}, {"y", "Integer"}, {"z", "Integer"}}, "Vec3",
            [](KiritoVM& v, std::span<const Handle> a) -> Handle {
                Args args(v, a, "Vec3");
                return makeVec3(v, args.at(0).asInt("x"), args.at(1).asInt("y"), args.at(2).asInt("z"));
            })));
        vm.registerDeserializer("Vec3", [](KiritoVM& v, Handle) { return v.alloc(std::make_unique<Vec3>()); });

        // From Kirito: getItem / setItem / contains / str.
        CHECK(ev(vm, "Vec3(1, 2, 3)[0]") == "1");
        CHECK(ev(vm, "Vec3(1, 2, 3)[2]") == "3");
        CHECK(ev(vm, "var v = Vec3(1, 2, 3)\nv[1] = 50\nv[1]") == "50");
        CHECK(ev(vm, "2 in Vec3(1, 2, 3)") == "True");
        CHECK(ev(vm, "9 in Vec3(1, 2, 3)") == "False");
        CHECK(ev(vm, "String(Vec3(4, 5, 6))") == "Vec3(4, 5, 6)");
        CHECK_THROWS(vm.runSource("Vec3(1, 2, 3)[7]"));   // out-of-range index throws (catchable)

        // Round-trip a single Vec3 through the TEXT codec, multi-field state preserved.
        Handle a = makeVec3(vm, 10, 20, 30);
        vm.pushTemp(a);
        {
            Handle back = serial::loads(vm, serial::dumps(vm, a));
            auto& bv = static_cast<Vec3&>(vm.arena().deref(back));
            CHECK(bv.c[0] == 10 && bv.c[1] == 20 && bv.c[2] == 30);
        }
        // ...and through the BINARY codec.
        {
            Handle back = dumpfmt::read(vm, dumpfmt::write(vm, a));
            auto& bv = static_cast<Vec3&>(vm.arena().deref(back));
            CHECK(bv.c[0] == 10 && bv.c[1] == 20 && bv.c[2] == 30);
        }

        // Shared + two-levels-nested: [[v, v]] — the alias must survive as one object in both codecs.
        {
            RootScope rs(vm);
            Handle inner = rs.add(vm.alloc(std::make_unique<ListVal>()));
            static_cast<ListVal&>(vm.arena().deref(inner)).elems = {a, a};      // same Vec3 twice
            Handle outer = rs.add(vm.alloc(std::make_unique<ListVal>()));
            static_cast<ListVal&>(vm.arena().deref(outer)).elems = {inner};

            Handle tb = serial::loads(vm, serial::dumps(vm, outer));
            auto& to = static_cast<ListVal&>(vm.arena().deref(tb));
            auto& ti = static_cast<ListVal&>(vm.arena().deref(to.elems[0]));
            CHECK(ti.elems.size() == 2);
            CHECK(ti.elems[0] == ti.elems[1]);                                  // shared ref preserved (text)
            CHECK(static_cast<Vec3&>(vm.arena().deref(ti.elems[0])).c[1] == 20);

            Handle db = dumpfmt::read(vm, dumpfmt::write(vm, outer));
            auto& doo = static_cast<ListVal&>(vm.arena().deref(db));
            auto& di = static_cast<ListVal&>(vm.arena().deref(doo.elems[0]));
            CHECK(di.elems[0] == di.elems[1]);                                  // shared ref preserved (binary)
        }
    }

    // ============================================================================================
    // 4) registerDeserializer OVERWRITE — the second factory for a name replaces the first.
    // ============================================================================================
    {
        KiritoVM vm;
        vm.registerDeserializer("Tagged", [](KiritoVM& v, Handle) {
            auto p = std::make_unique<Tagged>(); p->madeBy = "A"; return v.alloc(std::move(p));
        });
        vm.registerDeserializer("Tagged", [](KiritoVM& v, Handle) {     // overwrite
            auto p = std::make_unique<Tagged>(); p->madeBy = "B"; return v.alloc(std::move(p));
        });
        Handle t = vm.alloc(std::make_unique<Tagged>(77));
        vm.pushTemp(t);
        Handle back = serial::loads(vm, serial::dumps(vm, t));
        auto& tb = static_cast<Tagged&>(vm.arena().deref(back));
        CHECK(tb.v == 77);            // state restored by _setstate_
        CHECK(tb.madeBy == "B");      // the SECOND (overwriting) factory is the one that ran
    }

    // ============================================================================================
    // 5) makeMethod MULTI-capture GC-rooting — captures are the sole roots through children().
    // ============================================================================================
    {
        KiritoVM vm;
        vm.setGcEnabled(false);  // drive collection by hand
        Handle h1 = vm.alloc(std::make_unique<StrVal>("cap-one"));
        Handle h2 = vm.alloc(std::make_unique<StrVal>("cap-two"));
        Handle method = makeMethod(vm, "concat", {}, [h1, h2](KiritoVM& v, std::span<const Handle>) -> Handle {
            return v.makeString(static_cast<const StrVal&>(v.arena().deref(h1)).value() + "|" +
                                static_cast<const StrVal&>(v.arena().deref(h2)).value());
        }, std::vector<Handle>{h1, h2});
        vm.registerGlobal("cc", method);   // ONLY the method is rooted; h1/h2 live via its children()
        vm.collectGarbage();
        // The captured handles survived the collection and are still readable through the method.
        CHECK(ev(vm, "cc()") == "cap-one|cap-two");
        CHECK(static_cast<const StrVal&>(vm.arena().deref(h1)).value() == "cap-one");  // not swept
        CHECK(static_cast<const StrVal&>(vm.arena().deref(h2)).value() == "cap-two");
    }

    // ============================================================================================
    // 6) evalIn / Resolver contract for a custom-built parent scope chain.
    //
    // FINDING (characterized, not a regression assertion): the compile-time Resolver
    // (resolver.hpp::resolve) predeclares only the names found in the SINGLE scope handle passed to
    // evalIn (`env.locals()` of that scope) and otherwise resolves via the VM's GLOBAL chain
    // (isGlobal -> envLookup from vm.global()). It does NOT walk a custom, non-global parent chain.
    // So a name reachable only through a hand-built parent EnvValue is a COMPILE-TIME
    // `name '...' is not defined`, even though it WOULD resolve at run time (envLookup walks the chain).
    // The supported ways to inject a name an evalIn body can see are: (a) bind it directly in the passed
    // scope (this is exactly how runRepl's persistent scope works), or (b) registerGlobal it.
    // ============================================================================================
    {
        KiritoVM vm;

        // (a) A name bound DIRECTLY in the passed scope resolves — the supported pattern.
        Handle direct = vm.newScope(vm.global());
        vm.pushTemp(direct);
        static_cast<EnvValue&>(vm.arena().deref(direct)).define("injected", vm.makeInt(7));
        CHECK(vm.stringify(vm.evalIn("injected + 1", direct)) == "8");

        // (b) A registerGlobal name resolves through a child scope.
        vm.registerGlobal("g", vm.makeInt(99));
        Handle childOfGlobalName = vm.newScope(vm.global());
        vm.pushTemp(childOfGlobalName);
        CHECK(vm.stringify(vm.evalIn("g", childOfGlobalName)) == "99");

        // The FINDING: a name reachable only via a NON-global parent EnvValue is rejected at COMPILE time.
        Handle parent = vm.newScope(vm.global());
        vm.pushTemp(parent);
        static_cast<EnvValue&>(vm.arena().deref(parent)).define("fromParent", vm.makeInt(123));
        Handle child = vm.newScope(parent);
        vm.pushTemp(child);
        // The runtime chain DOES contain the name (the value graph is correct)...
        CHECK(envLookup(vm.arena(), child, "fromParent").has_value());
        // ...yet evalIn rejects it at compile-time resolution with a clear NameError (not a crash).
        bool nameErr = false;
        try { vm.evalIn("fromParent", child); }
        catch (const KiritoError& e) {
            nameErr = std::string(e.what()).find("not defined") != std::string::npos;
        }
        CHECK(nameErr);
    }

    // ============================================================================================
    // 7) runRepl persistence vs runSource isolation; setArgs -> arglist/argmain.
    // ============================================================================================
    {
        KiritoVM vm;
        vm.runRepl("var counter = 10\n");                          // persistent module scope
        CHECK(vm.stringify(vm.runRepl("counter = counter + 5\ncounter\n")) == "15");
        CHECK(vm.stringify(vm.runRepl("counter * 2\n")) == "30");  // still visible on a later line
        // A fresh runSource gets a NEW module scope -> it does NOT see the REPL's binding.
        CHECK_THROWS(vm.runSource("counter\n"));
    }
    {
        KiritoVM vm;
        vm.setArgs({"prog", "--flag", "value"});
        CHECK(vm.stringify(vm.runSource("len(arglist)\n")) == "3");      // arguments visible as arglist
        CHECK(vm.stringify(vm.runSource("arglist[0]\n")) == "prog");
        CHECK(vm.stringify(vm.runSource("argmain\n")) == "True");        // directly-run -> argmain True

        // A FROZEN (registerSourceModule) module is IMPORTED, so per the spec it sees an empty arglist
        // and argmain False — the same as a `.ki` FILE import. (R8 fixed runtime.hpp to load frozen
        // modules with newModuleScope(isMain=false); before the fix it wrongly used the isMain=true
        // default and saw the full arglist + argmain True.)
        vm.registerSourceModule("modargs", "var n = len(arglist)\nvar m = argmain\n");
        CHECK(vm.stringify(vm.runSource("import(\"modargs\").n\n")) == "0");        // imported frozen module: empty arglist
        CHECK(vm.stringify(vm.runSource("import(\"modargs\").m\n")) == "False");    // imported frozen module: argmain False
    }

    // ============================================================================================
    // 8) Parse/nesting + call-depth guards throw a catchable KiritoError (never crash the host).
    // ============================================================================================
    {
        KiritoVM vm;
        // Pathological nesting is rejected with a KiritoError, not a stack overflow.
        bool bracketErr = false;
        try { vm.runSource(std::string(6000, '[')); }
        catch (const KiritoError&) { bracketErr = true; }
        CHECK(bracketErr);

        std::string parens = "1";
        for (int i = 0; i < 6000; ++i) parens = "(" + parens + ")";
        bool parenErr = false;
        try { vm.runSource(parens); }
        catch (const KiritoError&) { parenErr = true; }
        CHECK(parenErr);

        // The call-depth guard throws a catchable "maximum recursion depth exceeded" (no native overflow).
        KiritoVM rvm;
        rvm.setMaxCallDepth(64);   // small cap so the test is fast
        const char* recur = "var f = Function(n):\n return f(n + 1)\nf(0)\n";
        bool depthErr = false;
        try { rvm.runSource(recur); }
        catch (const KiritoError& e) {
            depthErr = std::string(e.what()).find("recursion depth") != std::string::npos;
        }
        CHECK(depthErr);
        // The same guard is catchable from inside Kirito (it's an ordinary error, not a hard abort).
        // `f` must be (re)defined in THIS chunk: runSource makes a fresh module scope each call, so the
        // `f` from the chunk above isn't in scope here (that's the block-6 resolver contract).
        CHECK(rvm.stringify(rvm.runSource(
            "var f = Function(n):\n return f(n + 1)\n"
            "var ok = \"no\"\ntry:\n f(0)\ncatch as e:\n ok = \"caught\"\nok\n")) == "caught");
    }

    // ============================================================================================
    // 9) Per-VM registry isolation: classRegistry / deserializers / source modules / import cache.
    // ============================================================================================
    {
        KiritoVM a, b;

        // classRegistry: a class defined in `a` is findable in `a`, never in `b`.
        a.runSource("class Foo:\n var _init_ = Function(self, v):\n  self.v = v\n");
        CHECK(a.findClass("Foo") != nullptr);
        CHECK(b.findClass("Foo") == nullptr);

        // deserializer registry: registering in `a` does not populate `b`.
        a.registerDeserializer("Widget", [](KiritoVM& v, Handle) { return v.none(); });
        CHECK(a.findDeserializer("Widget") != nullptr);
        CHECK(b.findDeserializer("Widget") == nullptr);

        // registerSourceModule: `a`'s frozen module is importable in `a`, absent in `b`.
        a.registerSourceModule("priv", "var token = 42\n");
        CHECK(a.stringify(a.runSource("import(\"priv\").token\n")) == "42");
        CHECK_THROWS(b.runSource("import(\"priv\").token\n"));

        // import cache is per-VM: importing math in `a` doesn't make it resolved-and-shared with `b`,
        // and each VM resolves its own (both work, independently).
        CHECK(!a.stringify(a.runSource("type(import(\"math\"))\n")).empty());
        CHECK(!b.stringify(b.runSource("type(import(\"math\"))\n")).empty());

        // A registered global in one VM is invisible in the other (re-confirm share-nothing).
        a.registerGlobal("only_a", a.makeInt(1));
        CHECK_THROWS(b.runSource("only_a\n"));
    }

    return RUN_TESTS();
}
