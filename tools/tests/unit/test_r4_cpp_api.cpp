// Deep coverage of the C++-facing embedding / extension API and core internals — the parts best
// pinned from C++ rather than from .ki scripts. Complements test_value.cpp (the Value facade),
// test_embedding_extra.cpp (ModuleBuilder/makeMethod glue), test_protocol.cpp (built-in protocol
// slots), and test_arena.cpp/test_gc.cpp (arena + GC):
//
//  * value.hpp ergonomic facade end to end — val()/builders/Args, conversions, error propagation.
//  * native.hpp — a NativeModule (setup) + a NativeClass (overriding the protocol slots), registered
//    on a fresh VM and driven from Kirito source, including a signatured NativeFunction taking
//    KEYWORD ARGS bound out of order (NativeFunction::bindArgs) and `inspect` showing its signature.
//  * the Object protocol from C++ on a CUSTOM NativeClass (truthy/str/equals/hash; binary/unary/call/
//    getAttr/setAttr/getItem/setItem/iterate/length/contains/children).
//  * arena/handle basics + GC reachability (root keeps alive across collect; unrooted is reclaimed).
//  * multiple independent KiritoVM instances coexisting (share-nothing).
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string ev(KiritoVM& vm, const std::string& src) { return vm.stringify(vm.runSource(src)); }

// ---- a C++-authored module: a signatured fn (keyword args + inspect), a plain fn, a constant. ----
struct MathyMod : NativeModule {
    std::string name() const override { return "mathy"; }
    void setup(ModuleBuilder& m) override {
        // power(base, exp = 2) -> Integer — signatured, so callers may pass keywords / rely on default.
        m.fn("power", {{"base", "Integer"}, {"exp", "Integer", m.vm().makeInt(2)}}, "Integer",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 Args args(vm, a, "power");
                 int64_t base = args.at(0).asInt("base"), exp = args.at(1).asInt("exp");
                 int64_t r = 1;
                 for (int64_t i = 0; i < exp; ++i) r *= base;
                 return val(vm, r);
             });
        // sumlist(xs) -> Integer — iterate any iterable through the Value facade.
        m.fn("sumlist", {{"xs", "List"}}, "Integer", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "sumlist");
            int64_t s = 0;
            for (Value x : args.at(0).items()) s += x.asInt("element");
            return val(vm, s);
        });
        m.value("E3", val(m.vm(), 1000));
    }
};

// ---- a C++-authored value type exercising the FULL protocol from C++ -----------------------------
// A tiny fixed pair (a, b) supporting: str, truthy, equals, hash (hashable -> usable as a Set/Dict
// key), getAttr/setAttr, getItem/setItem, length, iterate, contains, call (returns a+b), unary neg,
// binary add (Pair+Pair), and children (GC reachability for its two boxed Integer handles).
struct Pair : NativeClass<Pair> {
    static constexpr const char* kTypeName = "Pair";
    Handle a, b;  // boxed Integer handles, owned-by-arena; we keep them reachable via children()
    Pair(Handle pa, Handle pb) : a(pa), b(pb) {}

    int64_t ai(KiritoVM& vm) const { return static_cast<const IntVal&>(vm.arena().deref(a)).value(); }
    int64_t bi(KiritoVM& vm) const { return static_cast<const IntVal&>(vm.arena().deref(b)).value(); }

    // str() gets no VM (it can't read the boxed ints), so a fixed tag — exercises that an override
    // takes precedence over the CRTP default and is what print/String show.
    std::string str(StringifyCtx&) const override { return "<a-pair>"; }

    bool truthy() const override { return true; }
    bool hashable() const override { return true; }
    std::size_t hash() const override { return 0xABCD; }  // constant — distinct Pairs collide but compare by value

    bool equals(const ObjectArena& arena, const Object& other) const override {
        const auto* o = dynamic_cast<const Pair*>(&other);
        if (!o) return false;
        return static_cast<const IntVal&>(arena.deref(a)).value() == static_cast<const IntVal&>(arena.deref(o->a)).value()
            && static_cast<const IntVal&>(arena.deref(b)).value() == static_cast<const IntVal&>(arena.deref(o->b)).value();
    }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        if (name == "a") return a;
        if (name == "b") return b;
        if (name == "swap")
            return makeMethod(vm, "swap", {}, [self](KiritoVM& mv, std::span<const Handle>) -> Handle {
                auto& p = static_cast<Pair&>(mv.arena().deref(self));
                return mv.alloc(std::make_unique<Pair>(p.b, p.a));
            }, std::vector<Handle>{self});
        return Object::getAttr(vm, self, name);
    }
    void setAttr(KiritoVM& vm, std::string_view name, Handle value) override {
        if (name == "a") { a = value; return; }
        if (name == "b") { b = value; return; }
        Object::setAttr(vm, name, value);
    }

    std::optional<int64_t> length(KiritoVM&) override { return 2; }
    std::optional<std::vector<Handle>> iterate(KiritoVM&) override {
        return std::vector<Handle>{a, b};
    }
    Handle getItem(KiritoVM& vm, std::span<const Handle> keys) override {
        int64_t i = static_cast<const IntVal&>(vm.arena().deref(keys[0])).value();
        if (i == 0) return a;
        if (i == 1) return b;
        throw KiritoError("Pair index out of range");
    }
    void setItem(KiritoVM& vm, std::span<const Handle> keys, Handle value) override {
        int64_t i = static_cast<const IntVal&>(vm.arena().deref(keys[0])).value();
        if (i == 0) { a = value; return; }
        if (i == 1) { b = value; return; }
        throw KiritoError("Pair index out of range");
    }
    bool contains(KiritoVM& vm, Handle value) override {
        int64_t v = static_cast<const IntVal&>(vm.arena().deref(value)).value();
        return v == ai(vm) || v == bi(vm);
    }
    Handle call(KiritoVM& vm, std::span<const Handle>) override { return vm.makeInt(ai(vm) + bi(vm)); }
    Handle unary(KiritoVM& vm, UnOp op, Handle) override {
        if (op == UnOp::Neg) return vm.alloc(std::make_unique<Pair>(vm.makeInt(-ai(vm)), vm.makeInt(-bi(vm))));
        return Object::unary(vm, op, Handle{});
    }
    Handle binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) override {
        if (op == BinOp::Add)
            if (auto* o = dynamic_cast<const Pair*>(&vm.arena().deref(rhs)))
                return vm.alloc(std::make_unique<Pair>(vm.makeInt(ai(vm) + o->ai(vm)),
                                                       vm.makeInt(bi(vm) + o->bi(vm))));
        return Object::binary(vm, op, self, rhs);
    }
    void children(std::vector<Handle>& out) const override { out.push_back(a); out.push_back(b); }
};

// Constructor so Kirito can build a Pair (signatured -> keyword args).
static Handle makePairCtor(KiritoVM& vm) {
    return vm.alloc(std::make_unique<NativeFunction>(
        "Pair", std::vector<NativeParam>{{"a", "Integer"}, {"b", "Integer"}}, "Pair",
        [](KiritoVM& v, std::span<const Handle> a) -> Handle {
            Args args(v, a, "Pair");
            return v.alloc(std::make_unique<Pair>(v.makeInt(args.at(0).asInt("a")),
                                                  v.makeInt(args.at(1).asInt("b"))));
        }));
}

int main() {
    // ============================================================================================
    // 1) value.hpp facade — conversions, builders, Args, and error propagation across runSource.
    // ============================================================================================
    {
        KiritoVM vm;

        // val() round-trips every primitive; reads are typed.
        CHECK(val(vm, 9).asInt() == 9);
        CHECK(val(vm, 2.5).asFloat() == 2.5);
        CHECK(val(vm, 7).asFloat() == 7.0);          // asFloat accepts an Integer
        CHECK(val(vm, std::string("hi")).asString() == "hi");
        CHECK(val(vm, true).asBool() == true);
        CHECK(none(vm).isNone());

        // a typed-read mismatch is a clear, catchable KiritoError (not a crash).
        CHECK_THROWS(val(vm, "x").asInt("n"));
        CHECK_THROWS(val(vm, 1).asString("s"));

        // builders + facade reads (List/Dict/Set), and stringify of a nested String (repr form).
        Value lst = List(vm).add(1).add(2).add("three").build();
        CHECK(lst.isList() && lst.len() == 3);
        CHECK(lst.at(0).asInt() == 1 && lst.at(-1).asString() == "three");
        CHECK(vm.stringify(lst) == "[1, 2, 'three']");

        Value d = Dict(vm).set("k", 5).set("name", "Ada").build();
        CHECK(d.isDict() && d.get("k").asInt() == 5 && d.get("name").asString() == "Ada");
        CHECK(d.has("k") && !d.has("absent"));
        CHECK(d.get("absent", val(vm, -1)).asInt() == -1);

        Value s = Set(vm).add(1).add(1).add(2).build();
        CHECK(s.isSet() && s.len() == 2);

        // a native function authored against Args + the facade, driven from Kirito.
        vm.registerGlobal("addmul", vm.alloc(std::make_unique<NativeFunction>(
            "addmul", [](KiritoVM& kv, std::span<const Handle> raw) -> Handle {
                Args a(kv, raw, "addmul");
                int64_t x = a.at(0).asInt("x");
                int64_t y = a.opt(1, val(kv, 1)).asInt("y");   // y defaults to 1 when absent
                return val(kv, (x + y) * 2);
            })));
        CHECK(ev(vm, "addmul(3, 4)") == "14");
        CHECK(ev(vm, "addmul(3)") == "8");                 // opt() default used
        CHECK_THROWS(vm.runSource("addmul()"));            // missing required arg -> clear error
        CHECK_THROWS(vm.runSource("addmul(\"nope\")"));    // bad type propagates as KiritoError
    }

    // ============================================================================================
    // 2) NativeModule with a signatured fn: keyword args bound out of order, defaults, inspect.
    // ============================================================================================
    {
        KiritoVM vm;
        vm.install<MathyMod>();

        CHECK(ev(vm, "import(\"mathy\").power(3)") == "9");                  // exp defaults to 2
        CHECK(ev(vm, "import(\"mathy\").power(2, 5)") == "32");             // positional
        CHECK(ev(vm, "import(\"mathy\").power(base = 2, exp = 10)") == "1024");
        CHECK(ev(vm, "import(\"mathy\").power(exp = 3, base = 2)") == "8");  // OUT OF ORDER keywords
        CHECK(ev(vm, "import(\"mathy\").sumlist([1, 2, 3, 4])") == "10");
        CHECK(ev(vm, "import(\"mathy\").E3") == "1000");

        // bad keyword / duplicate / wrong type all throw.
        CHECK_THROWS(vm.runSource("import(\"mathy\").power(2, bogus = 1)"));    // unknown keyword
        CHECK_THROWS(vm.runSource("import(\"mathy\").power(2, base = 3)"));     // duplicate 'base'
        CHECK_THROWS(vm.runSource("import(\"mathy\").power(\"x\")"));           // wrong type

        // inspect renders the declared signature (name: Type, default, return type).
        std::string desc = ev(vm, "inspect(import(\"mathy\").power)");
        CHECK(desc.find("power(") != std::string::npos);
        CHECK(desc.find("base: Integer") != std::string::npos);
        CHECK(desc.find("exp: Integer = 2") != std::string::npos);
        CHECK(desc.find("-> Integer") != std::string::npos);
    }

    // ============================================================================================
    // 3) bindArgs directly (the keyword-binding core a signatured native uses).
    // ============================================================================================
    {
        KiritoVM vm;
        NativeFunction nf("f", std::vector<NativeParam>{{"a", "Integer"}, {"b", "Integer", vm.makeInt(99)}},
                          "Integer", [](KiritoVM&, std::span<const Handle>) -> Handle { return Handle{}; });
        CHECK(nf.hasSignature());

        // positional + keyword, bound out of order; the absent param takes its default.
        std::array<Handle, 1> pos{vm.makeInt(1)};
        std::array<NamedArg, 1> named{NamedArg{"b", vm.makeInt(2)}};
        std::vector<Handle> bound = nf.bindArgs(pos, named);
        CHECK(bound.size() == 2);
        CHECK(static_cast<const IntVal&>(vm.arena().deref(bound[0])).value() == 1);
        CHECK(static_cast<const IntVal&>(vm.arena().deref(bound[1])).value() == 2);

        // default fills the missing 'b'.
        std::vector<Handle> defaulted = nf.bindArgs(pos, {});
        CHECK(static_cast<const IntVal&>(vm.arena().deref(defaulted[1])).value() == 99);

        // a keyword for a positional, alone.
        std::array<NamedArg, 1> aOnly{NamedArg{"a", vm.makeInt(7)}};
        std::vector<Handle> kwpos = nf.bindArgs({}, aOnly);
        CHECK(static_cast<const IntVal&>(vm.arena().deref(kwpos[0])).value() == 7);
        CHECK(static_cast<const IntVal&>(vm.arena().deref(kwpos[1])).value() == 99);

        // error paths: unknown keyword, duplicate, too many positionals, missing required.
        std::array<NamedArg, 1> bad{NamedArg{"zzz", vm.makeInt(0)}};
        CHECK_THROWS(nf.bindArgs(pos, bad));
        std::array<NamedArg, 1> dupA{NamedArg{"a", vm.makeInt(0)}};
        CHECK_THROWS(nf.bindArgs(pos, dupA));    // 'a' given positionally and by keyword
        std::array<Handle, 3> tooMany{vm.makeInt(1), vm.makeInt(2), vm.makeInt(3)};
        CHECK_THROWS(nf.bindArgs(tooMany, {}));
        CHECK_THROWS(nf.bindArgs({}, {}));       // missing required 'a' (no default)
    }

    // ============================================================================================
    // 4) The full Object protocol on a custom NativeClass (Pair), driven from Kirito + from C++.
    // ============================================================================================
    {
        KiritoVM vm;
        vm.install<MathyMod>();  // unused here but proves coexistence with a registered module
        vm.registerGlobal("Pair", makePairCtor(vm));

        // attributes + setAttr
        CHECK(ev(vm, "Pair(3, 4).a") == "3");
        CHECK(ev(vm, "Pair(3, 4).b") == "4");
        CHECK(ev(vm, "var p = Pair(1, 2)\np.a = 9\np.a") == "9");
        // getItem / setItem
        CHECK(ev(vm, "Pair(5, 6)[0]") == "5");
        CHECK(ev(vm, "Pair(5, 6)[1]") == "6");
        CHECK(ev(vm, "var q = Pair(1, 1)\nq[1] = 8\nq[1]") == "8");
        CHECK_THROWS(vm.runSource("Pair(1, 2)[5]"));
        // length / iterate (for-loop) / contains (`in`)
        CHECK(ev(vm, "len(Pair(7, 8))") == "2");
        CHECK(ev(vm, "var s = 0\nfor x in Pair(10, 20):\n    s = s + x\ns") == "30");
        CHECK(ev(vm, "10 in Pair(10, 20)") == "True");
        CHECK(ev(vm, "99 in Pair(10, 20)") == "False");
        // call (obj(...)) / unary neg / binary add
        CHECK(ev(vm, "Pair(3, 4)()") == "7");
        CHECK(ev(vm, "var n = -Pair(3, 4)\nn.a") == "-3");
        CHECK(ev(vm, "var c = Pair(1, 2) + Pair(10, 20)\nc.a") == "11");
        CHECK(ev(vm, "(Pair(1, 2) + Pair(10, 20)).b") == "22");
        // method via makeMethod (keyword-arg aware; here nullary)
        CHECK(ev(vm, "Pair(1, 2).swap().a") == "2");
        // truthy + type
        CHECK(ev(vm, "Bool(Pair(0, 0))") == "True");   // custom truthy is always true
        CHECK(ev(vm, "type(Pair(1, 2))") == "Pair");
        // equals (value equality), and hashable so it works as a Set element / Dict key
        CHECK(ev(vm, "Pair(1, 2) == Pair(1, 2)") == "True");
        CHECK(ev(vm, "Pair(1, 2) == Pair(1, 3)") == "False");
        CHECK(ev(vm, "var st = Set()\nst.add(Pair(1, 2))\nst.add(Pair(1, 2))\nlen(st)") == "1");  // dedup by equals+hash
        CHECK(ev(vm, "Pair(1, 2) in {Pair(1, 2)}") == "True");

        // unsupported slots fall back to the base "unsupported" error.
        CHECK_THROWS(vm.runSource("Pair(1, 2).nope"));        // no such attribute
        CHECK_THROWS(vm.runSource("Pair(1, 2) - Pair(3, 4)"));// Sub not overridden

        // ---- exercise the same slots directly from C++ (the embedder contract) ----
        Handle ph = vm.alloc(std::make_unique<Pair>(vm.makeInt(11), vm.makeInt(22)));
        vm.pushTemp(ph);  // keep it rooted across the allocations below
        Object& po = vm.arena().deref(ph);
        CHECK(po.typeName() == "Pair");
        CHECK(po.truthy() == true);
        CHECK(po.hashable() == true);
        CHECK(vm.stringify(ph) == "<a-pair>");   // the overridden str()
        auto len = po.length(vm);
        CHECK(len.has_value() && *len == 2);
        auto it = po.iterate(vm);
        CHECK(it.has_value() && it->size() == 2);
        CHECK(po.contains(vm, vm.makeInt(11)) == true);
        CHECK(po.contains(vm, vm.makeInt(0)) == false);
        std::array<Handle, 1> k0{vm.makeInt(0)};
        CHECK(vm.stringify(po.getItem(vm, k0)) == "11");
        CHECK(vm.stringify(po.call(vm, {})) == "33");
        // unary neg returns a new (negated) Pair; check its first component.
        Handle negh = po.unary(vm, UnOp::Neg, ph);
        CHECK(static_cast<const Pair&>(vm.arena().deref(negh)).ai(vm) == -11);
        // value-equality from C++
        Handle ph2 = vm.alloc(std::make_unique<Pair>(vm.makeInt(11), vm.makeInt(22)));
        CHECK(po.equals(vm.arena(), vm.arena().deref(ph2)) == true);
        Handle ph3 = vm.alloc(std::make_unique<Pair>(vm.makeInt(11), vm.makeInt(0)));
        CHECK(po.equals(vm.arena(), vm.arena().deref(ph3)) == false);
        // children() enumerates the two boxed handles (GC/serialization contract)
        std::vector<Handle> kids;
        po.children(kids);
        CHECK(kids.size() == 2);
    }

    // ============================================================================================
    // 5) Arena / handle basics + GC reachability.
    // ============================================================================================
    {
        KiritoVM vm;
        // a fabricated handle names no live slot -> rejected, not silently dereferenced.
        CHECK_THROWS(vm.arena().deref(Handle{123456, 0}));

        // GC: a globally-rooted Pair survives an explicit collection; an unrooted one is reclaimed.
        vm.setGcEnabled(false);  // drive collection manually
        Handle kept = vm.alloc(std::make_unique<Pair>(vm.makeInt(1), vm.makeInt(2)));
        vm.registerGlobal("kept", kept);  // reachable from the module scope
        Handle dropped = vm.alloc(std::make_unique<Pair>(vm.makeInt(3), vm.makeInt(4)));
        (void)dropped;  // no root holds it

        std::size_t before = vm.liveCount();
        vm.collectGarbage();
        std::size_t after = vm.liveCount();
        CHECK(after <= before);                 // something was reclaimed (or at least nothing grew)
        // 'kept' (and its two boxed ints, kept alive via children()) is still usable.
        Object& keptObj = vm.arena().deref(kept);
        CHECK(keptObj.typeName() == "Pair");
        auto klen = keptObj.length(vm);
        CHECK(klen.has_value() && *klen == 2);
        // the dropped handle now dangles (its slot was swept / generation bumped).
        CHECK_THROWS(vm.arena().deref(dropped));

        // a RootScope keeps an intermediate alive across a collection, then releases it.
        std::size_t markLive;
        {
            RootScope rs(vm);
            Handle tmp = rs.add(vm.alloc(std::make_unique<Pair>(vm.makeInt(7), vm.makeInt(8))));
            vm.collectGarbage();
            CHECK(vm.arena().deref(tmp).typeName() == "Pair");  // survived while rooted
            markLive = vm.liveCount();
        }
        vm.collectGarbage();  // rs is gone -> tmp now collectable
        CHECK(vm.liveCount() <= markLive);
    }

    // ============================================================================================
    // 6) Multiple independent KiritoVM instances coexist (share-nothing).
    // ============================================================================================
    {
        KiritoVM a, b;
        a.install<MathyMod>();
        a.registerGlobal("Pair", makePairCtor(a));
        // a sees its module + global; b saw neither.
        CHECK(a.stringify(a.runSource("import(\"mathy\").power(2, 4)")) == "16");
        CHECK(a.stringify(a.runSource("Pair(1, 2).a")) == "1");
        CHECK_THROWS(b.runSource("import(\"mathy\").power(2, 4)"));
        CHECK_THROWS(b.runSource("Pair(1, 2)"));
        // b has its own clean state; a global in one VM never leaks to the other.
        b.registerGlobal("only_b", b.makeInt(42));
        CHECK(b.stringify(b.runSource("only_b")) == "42");
        CHECK_THROWS(a.runSource("only_b"));
        // handles from one VM's arena are meaningless in the other (their arenas are separate).
        CHECK(a.liveCount() != 0 && b.liveCount() != 0);
    }

    return RUN_TESTS();
}
