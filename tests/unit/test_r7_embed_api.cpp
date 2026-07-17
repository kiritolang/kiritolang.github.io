// Round-7 (R7) embedding / extension API audit — pinning C++-side contracts that the existing
// embedding tests (test_r4_cpp_api / test_value{,_extra} / test_embedding_extra / test_protocol /
// test_serde / test_arena / test_gc) do NOT already cover. Each block targets a genuinely-uncovered
// surface; see the header comment above each.
//
//   1. Arena ABA / generation-reuse safety: after a slot is freed and RECYCLED for a new object, the
//      OLD handle to that slot is rejected (stale generation) while the new handle works — the
//      use-after-free guard the model promises (handle = slot + generation).
//   2. NativeClass `slice` slot: a custom type's slice(start, stop, step) override driven from Kirito
//      (obj[a:b:c]) AND from C++ — the protocol test only slices built-ins.
//   3. NativeClass variadic getItem/setItem with MULTIPLE keys (m[i, j] / m[i, j] = v).
//   4. NativeClass::inspectMembers() surfaced by inspect() on a custom native type.
//   5. A C++ KiritoError thrown from a NON-call protocol slot (binary / getItem / iterate) is a
//      CATCHABLE Kirito error; and a Kirito `throw` surfaces back to C++ as a KiritoError with a span.
//   6. GC auxiliary roots (pushAuxRoots/popAuxRoots) keep an external operand vector's handles alive;
//      and the RootScope / pushTemp / tempMark / popTempTo primitives compose (nested + interleaved).
//   7. registerGlobal SHADOWS a builtin (per-VM), and is not visible in a second VM.
//   8. Two independent KiritoVMs: collecting one never disturbs the other's live set (GC isolation).
//   9. KiritoDispatcher embedding lifecycle: mainVM() is configured (parallel present), survives a
//      destroyed-then-recreated dispatcher, and two dispatchers are fully isolated.
//  10. alloc() bypasses small-int interning (a fresh boxed Integer is a distinct object from the
//      interned one) yet compares and hashes equal — the embedder contract for raw alloc vs makeInt.
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

// ---- a custom NativeClass exercising slice, multi-key getItem/setItem, inspectMembers, and a slot
// ---- that throws a C++ KiritoError (binary/getItem/iterate) ------------------------------------
// A tiny fixed 2x2 grid of Integers addressed as g[i, j]; g[a:b] returns a List of the flat backing
// store sliced; iterate() throws (to prove a non-call slot's C++ throw is catchable); binary(+) is
// unsupported on purpose so the default "unsupported" error path is reachable.
struct Grid : NativeClass<Grid> {
    static constexpr const char* kTypeName = "Grid";
    std::array<Handle, 4> cells;  // row-major 2x2, boxed Integer handles (rooted via children())
    explicit Grid(std::array<Handle, 4> c) : cells(c) {}

    std::string str(StringifyCtx&) const override { return "<Grid 2x2>"; }

    std::vector<std::string> inspectMembers() const override {
        return {"get(i, j) -> Integer", "rows: Integer"};
    }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        if (name == "rows") return vm.makeInt(2);
        return Object::getAttr(vm, self, name);
    }

    // Multi-key indexing: g[i, j].
    Handle getItem(KiritoVM& vm, std::span<const Handle> keys) override {
        if (keys.size() != 2) throw KiritoError("Grid needs two indices");
        int64_t i = static_cast<const IntVal&>(vm.arena().deref(keys[0])).value();
        int64_t j = static_cast<const IntVal&>(vm.arena().deref(keys[1])).value();
        if (i < 0 || i > 1 || j < 0 || j > 1) throw KiritoError("Grid index out of range");
        return cells[static_cast<std::size_t>(i * 2 + j)];
    }
    void setItem(KiritoVM& vm, std::span<const Handle> keys, Handle value) override {
        if (keys.size() != 2) throw KiritoError("Grid needs two indices");
        int64_t i = static_cast<const IntVal&>(vm.arena().deref(keys[0])).value();
        int64_t j = static_cast<const IntVal&>(vm.arena().deref(keys[1])).value();
        if (i < 0 || i > 1 || j < 0 || j > 1) throw KiritoError("Grid index out of range");
        cells[static_cast<std::size_t>(i * 2 + j)] = value;
    }
    // Slice over the flat 4-cell backing store; returns a List. start/stop/step are Integer-or-None.
    Handle slice(KiritoVM& vm, Handle start, Handle stop, Handle step) override {
        std::vector<int64_t> idx = sliceIndices(vm, 4, start, stop, step);
        List out(vm);
        for (int64_t k : idx) out.push(cells[static_cast<std::size_t>(k)]);
        return out;
    }
    // A non-call slot whose C++ throw must surface as a catchable Kirito error.
    std::optional<std::vector<Handle>> iterate(KiritoVM&) override {
        throw KiritoError("Grid is not iterable (by design)");
    }
    void children(std::vector<Handle>& out) const override {
        for (Handle h : cells) out.push_back(h);
    }
};

static Handle makeGridCtor(KiritoVM& vm) {
    return vm.alloc(std::make_unique<NativeFunction>(
        "Grid", std::vector<NativeParam>{{"a", "Integer"}, {"b", "Integer"},
                                         {"c", "Integer"}, {"d", "Integer"}}, "Grid",
        [](KiritoVM& v, std::span<const Handle> a) -> Handle {
            Args args(v, a, "Grid");
            std::array<Handle, 4> c{v.makeInt(args.at(0).asInt("a")), v.makeInt(args.at(1).asInt("b")),
                                    v.makeInt(args.at(2).asInt("c")), v.makeInt(args.at(3).asInt("d"))};
            return v.alloc(std::make_unique<Grid>(c));
        }));
}

int main() {
    // ============================================================================================
    // 1) Arena ABA / generation-reuse safety — the use-after-free guard.
    // ============================================================================================
    {
        KiritoVM vm;
        vm.setGcEnabled(false);  // drive collection by hand so the slot reuse is deterministic

        // Allocate an unrooted object, capture its handle, then collect so its slot is freed.
        Handle old = vm.alloc(std::make_unique<ListVal>());
        uint32_t oldSlot = old.slot, oldGen = old.generation;
        vm.collectGarbage();                       // nothing roots `old` -> swept, generation bumped
        CHECK_THROWS(vm.arena().deref(old));       // the captured handle now dangles

        // Force the SAME slot to be recycled for a brand-new object: the arena pushes freed slots to a
        // free-list and pops the most-recent first, so the next alloc reuses oldSlot at a higher gen.
        Handle fresh = vm.alloc(std::make_unique<StrVal>("reused"));
        CHECK(fresh.slot == oldSlot);              // slot really was recycled (ABA setup)
        CHECK(fresh.generation != oldGen);         // ...but at a new generation
        // The OLD handle (same slot, stale generation) is STILL rejected — not silently the new value.
        CHECK_THROWS(vm.arena().deref(old));
        // The NEW handle works and names the new object.
        CHECK(vm.arena().deref(fresh).kind() == ValueKind::String);
        CHECK(static_cast<const StrVal&>(vm.arena().deref(fresh)).value() == "reused");
        // A fabricated handle at the right slot but a wrong (too-low) generation is rejected too.
        CHECK_THROWS(vm.arena().deref(Handle{oldSlot, oldGen}));
        // markIfUnmarked on a stale handle is a no-op (returns false), never marks the live object.
        CHECK(vm.arena().markIfUnmarked(old) == false);
    }

    // ============================================================================================
    // 1b) The default `Handle{}` sentinel never aliases a real value. Generation 0 is reserved, so
    //     even the very first object allocated on a fresh VM/arena has a non-zero generation — a
    //     default handle can therefore be used as a reliable "no value" marker (as class selfHandle,
    //     KiritoError::value, and the no-owner call path all do).
    // ============================================================================================
    {
        ObjectArena arena;
        Handle first = arena.alloc(std::make_unique<ListVal>());
        CHECK(first.slot == 0);                 // the first slot really is index 0...
        CHECK(first.generation != 0);           // ...but its generation is NOT 0, so first != Handle{}
        CHECK(!(first == Handle{}));            // an actual object is never equal to the sentinel
        // The sentinel itself dangles (slot 0 IS occupied, but at the reserved generation 0 -> mismatch).
        CHECK_THROWS(arena.deref(Handle{}));
        CHECK(arena.markIfUnmarked(Handle{}) == false);
    }

    // ============================================================================================
    // 2) NativeClass `slice` slot — from Kirito (obj[a:b:c]) and from C++.
    // ============================================================================================
    {
        KiritoVM vm;
        vm.registerGlobal("Grid", makeGridCtor(vm));

        // From Kirito: a 2x2 grid [10,20,30,40] sliced over its flat store.
        CHECK(ev(vm, "Grid(10, 20, 30, 40)[0:2]") == "[10, 20]");
        CHECK(ev(vm, "Grid(10, 20, 30, 40)[1:]") == "[20, 30, 40]");
        CHECK(ev(vm, "Grid(10, 20, 30, 40)[::2]") == "[10, 30]");
        CHECK(ev(vm, "Grid(10, 20, 30, 40)[::-1]") == "[40, 30, 20, 10]");   // negative step
        CHECK(ev(vm, "Grid(10, 20, 30, 40)[10:20]") == "[]");               // out-of-range -> empty

        // From C++: call slice() directly with Integer-or-None bounds.
        Handle g = vm.alloc(std::make_unique<Grid>(std::array<Handle, 4>{
            vm.makeInt(1), vm.makeInt(2), vm.makeInt(3), vm.makeInt(4)}));
        vm.pushTemp(g);
        Object& go = vm.arena().deref(g);
        Handle sl = go.slice(vm, vm.makeInt(1), vm.makeInt(3), vm.none());
        CHECK(vm.stringify(sl) == "[2, 3]");
        // a zero step throws (sliceIndices contract), surfaced cleanly.
        CHECK_THROWS(go.slice(vm, vm.none(), vm.none(), vm.makeInt(0)));
    }

    // ============================================================================================
    // 3) NativeClass variadic getItem / setItem with MULTIPLE keys (m[i, j]).
    // ============================================================================================
    {
        KiritoVM vm;
        vm.registerGlobal("Grid", makeGridCtor(vm));
        CHECK(ev(vm, "Grid(1, 2, 3, 4)[0, 0]") == "1");
        CHECK(ev(vm, "Grid(1, 2, 3, 4)[0, 1]") == "2");
        CHECK(ev(vm, "Grid(1, 2, 3, 4)[1, 1]") == "4");
        CHECK(ev(vm, "var g = Grid(1, 2, 3, 4)\ng[1, 0] = 99\ng[1, 0]") == "99");
        // wrong arity / out of range throw (and are catchable).
        CHECK_THROWS(vm.runSource("Grid(1, 2, 3, 4)[0]"));        // one index -> "needs two indices"
        CHECK_THROWS(vm.runSource("Grid(1, 2, 3, 4)[2, 0]"));     // out of range
        CHECK(ev(vm, "var ok = \"no\"\ntry:\n Grid(1,2,3,4)[5,5]\ncatch as e:\n ok = \"c\"\nok") == "c");

        // From C++: setItem then getItem with a 2-key span.
        Handle g = vm.alloc(std::make_unique<Grid>(std::array<Handle, 4>{
            vm.makeInt(0), vm.makeInt(0), vm.makeInt(0), vm.makeInt(0)}));
        vm.pushTemp(g);
        Object& go = vm.arena().deref(g);
        std::array<Handle, 2> key{vm.makeInt(0), vm.makeInt(1)};
        go.setItem(vm, key, vm.makeInt(77));
        CHECK(vm.stringify(go.getItem(vm, key)) == "77");
    }

    // ============================================================================================
    // 4) NativeClass::inspectMembers() surfaced by inspect().
    // ============================================================================================
    {
        KiritoVM vm;
        vm.registerGlobal("Grid", makeGridCtor(vm));
        std::string desc = ev(vm, "inspect(Grid(1, 2, 3, 4))");
        CHECK(desc.find("get(i, j) -> Integer") != std::string::npos);
        CHECK(desc.find("rows: Integer") != std::string::npos);
        // The custom attribute the inspect line advertises actually reads.
        CHECK(ev(vm, "Grid(1, 2, 3, 4).rows") == "2");
    }

    // ============================================================================================
    // 5) A C++ KiritoError from a NON-call slot is catchable; a Kirito throw surfaces to C++.
    // ============================================================================================
    {
        KiritoVM vm;
        vm.registerGlobal("Grid", makeGridCtor(vm));

        // iterate() throws in C++ -> a `for` over a Grid throws, catchable from Kirito.
        CHECK_THROWS(vm.runSource("for x in Grid(1, 2, 3, 4):\n pass\n"));
        CHECK(ev(vm, "var ok = \"no\"\ntry:\n for x in Grid(1,2,3,4):\n  pass\ncatch as e:\n ok = \"caught\"\nok")
              == "caught");
        // binary(+) is not overridden -> the base "unsupported binary operator" error path, catchable.
        CHECK_THROWS(vm.runSource("Grid(1, 2, 3, 4) + Grid(1, 2, 3, 4)"));
        {
            KiritoVM v2;
            v2.registerGlobal("Grid", makeGridCtor(v2));
            bool threw = false;
            try { v2.runSource("Grid(1,2,3,4) + 1"); }
            catch (const KiritoError& e) {
                threw = std::string(e.what()).find("binary operator") != std::string::npos;
            }
            CHECK(threw);
        }

        // A Kirito `throw` of a value surfaces to the embedder as a KiritoError carrying a span.
        {
            bool threw = false;
            try { vm.runSource("var x = 1\nthrow \"boom\"\n"); }
            catch (const KiritoError& e) {
                threw = true;
                CHECK(std::string(e.what()).find("boom") != std::string::npos);
                CHECK(e.span.line == 2);                  // the throw is on line 2
            }
            CHECK(threw);
        }
        // A native-thrown error escaping an uncaught call also reaches C++ as KiritoError.
        CHECK_THROWS(vm.runSource("import(\"math\").sqrt(-1)"));
    }

    // ============================================================================================
    // 6) GC auxiliary roots + the temp-root primitives compose.
    // ============================================================================================
    {
        KiritoVM vm;
        vm.setGcEnabled(false);

        // An external operand vector (as the bytecode VM keeps) registered via pushAuxRoots keeps its
        // handles alive across a collection — even with NO other root holding them.
        std::vector<Handle> operands;
        operands.push_back(vm.alloc(std::make_unique<ListVal>()));
        operands.push_back(vm.alloc(std::make_unique<StrVal>("aux")));
        vm.pushAuxRoots(&operands);
        vm.collectGarbage();                                   // survives only because of the aux region
        CHECK(vm.arena().deref(operands[0]).kind() == ValueKind::List);
        CHECK(static_cast<const StrVal&>(vm.arena().deref(operands[1])).value() == "aux");
        vm.popAuxRoots();
        Handle wasAux1 = operands[1];
        operands.clear();                                      // drop the only references
        vm.collectGarbage();                                   // now reclaimable
        CHECK_THROWS(vm.arena().deref(wasAux1));               // the de-registered handle dangles
    }
    {
        // tempMark / pushTemp / popTempTo compose with nested RootScopes: a handle rooted at an OUTER
        // mark survives collections taken inside an inner scope, and is freed only after the outer pop.
        KiritoVM vm;
        vm.setGcEnabled(false);
        std::size_t outerMark = vm.tempMark();
        Handle outer = vm.alloc(std::make_unique<StrVal>("outer"));
        vm.pushTemp(outer);
        Handle inner;
        {
            RootScope rs(vm);
            inner = rs.add(vm.alloc(std::make_unique<StrVal>("inner")));   // add() returns the handle
            CHECK(inner.slot != 0);
            vm.collectGarbage();
            CHECK(static_cast<const StrVal&>(vm.arena().deref(inner)).value() == "inner");
            CHECK(static_cast<const StrVal&>(vm.arena().deref(outer)).value() == "outer");
        }
        vm.collectGarbage();                                   // inner's RootScope gone -> inner freed
        CHECK_THROWS(vm.arena().deref(inner));
        CHECK(static_cast<const StrVal&>(vm.arena().deref(outer)).value() == "outer");  // outer survives
        vm.popTempTo(outerMark);                               // release outer
        vm.collectGarbage();
        CHECK_THROWS(vm.arena().deref(outer));
    }

    // ============================================================================================
    // 7) registerGlobal SHADOWS a builtin per-VM; not visible in another VM.
    // ============================================================================================
    {
        KiritoVM vm;
        // `len` is a builtin; rebind it to a constant-returning native and confirm the shadow wins.
        vm.registerGlobal("len", vm.alloc(std::make_unique<NativeFunction>(
            "len", [](KiritoVM& v, std::span<const Handle>) -> Handle { return v.makeInt(-1); })));
        CHECK(ev(vm, "len([1, 2, 3])") == "-1");               // the shadow, not the builtin (==3)

        KiritoVM clean;
        CHECK(ev(clean, "len([1, 2, 3])") == "3");             // a fresh VM keeps the real builtin
    }

    // ============================================================================================
    // 8) GC isolation between two independent VMs: collecting one never disturbs the other.
    // ============================================================================================
    {
        KiritoVM a, b;
        a.setGcEnabled(false);
        b.setGcEnabled(false);
        Handle bKept = b.alloc(std::make_unique<ListVal>());
        static_cast<ListVal&>(b.arena().deref(bKept)).elems = {b.makeInt(1), b.makeInt(2)};
        b.registerGlobal("kept", bKept);

        // Allocate churn in `a` and collect `a` aggressively; `b`'s live set is untouched.
        std::size_t bLiveBefore = b.liveCount();
        for (int i = 0; i < 100; ++i) (void)a.alloc(std::make_unique<ListVal>());
        a.collectGarbage();
        CHECK(b.liveCount() == bLiveBefore);                   // a's GC didn't touch b
        CHECK(static_cast<ListVal&>(b.arena().deref(bKept)).elems.size() == 2);  // b's value intact

        // And b's handle is meaningless in a's arena (separate slot spaces) — guarded, not UB.
        // (Use a slot index a's arena certainly lacks would be flaky; instead confirm independence by
        // value: a never saw `kept`.)
        CHECK_THROWS(a.runSource("kept"));
    }

    // ============================================================================================
    // 9) KiritoDispatcher embedding lifecycle: configured mainVM, recreate, isolation.
    // ============================================================================================
    {
        // mainVM() is fully configured: `parallel` is importable (a bare VM can't), and ordinary code
        // runs. The dispatcher is the documented embedding entry point.
        KiritoDispatcher disp;
        KiritoVM& vm = disp.mainVM();
        CHECK(ev(vm, "var x = 6 * 7\nx") == "42");
        CHECK(!ev(vm, "type(import(\"parallel\"))").empty());  // parallel present (no throw)
        // A bare VM has NO parallel — the documented difference.
        {
            KiritoVM bare;
            CHECK_THROWS(bare.runSource("import(\"parallel\")"));
        }
    }
    {
        // A destroyed dispatcher leaves no global state behind; a fresh one starts clean.
        {
            KiritoDispatcher d1;
            d1.mainVM().registerGlobal("only_d1", d1.mainVM().makeInt(7));
            CHECK(d1.mainVM().stringify(d1.mainVM().runSource("only_d1")) == "7");
        }  // ~d1
        KiritoDispatcher d2;
        CHECK_THROWS(d2.mainVM().runSource("only_d1"));        // no leak across dispatchers
    }
    {
        // Two coexisting dispatchers are isolated (no shared values/globals).
        KiritoDispatcher da, db;
        da.mainVM().registerGlobal("tag", da.mainVM().makeString("A"));
        db.mainVM().registerGlobal("tag", db.mainVM().makeString("B"));
        CHECK(da.mainVM().stringify(da.mainVM().runSource("tag")) == "A");
        CHECK(db.mainVM().stringify(db.mainVM().runSource("tag")) == "B");
    }

    // ============================================================================================
    // 10) alloc() bypasses small-int interning; a fresh boxed Integer is a distinct object but equal.
    // ============================================================================================
    {
        KiritoVM vm;
        Handle interned = vm.makeInt(5);                       // within [-256, 256] -> interned
        CHECK(vm.makeInt(5) == interned);                      // makeInt returns the SAME handle
        Handle boxed = vm.alloc(std::make_unique<IntVal>(5));  // raw alloc -> a NEW object/slot
        CHECK(!(boxed == interned));                           // a distinct handle
        CHECK(&vm.arena().deref(boxed) != &vm.arena().deref(interned));  // distinct objects
        // ...yet they are value-equal and hash-equal (the contract that lets them share Set/Dict keys).
        CHECK(vm.arena().deref(boxed).equals(vm.arena(), vm.arena().deref(interned)));
        CHECK(vm.arena().deref(boxed).hash() == vm.arena().deref(interned).hash());
        // A large value is NOT interned: two makeInt calls give two distinct handles.
        Handle big1 = vm.makeInt(1'000'000), big2 = vm.makeInt(1'000'000);
        CHECK(!(big1 == big2));
        CHECK(vm.arena().deref(big1).equals(vm.arena(), vm.arena().deref(big2)));
        // makeBool interns True/False (singletons).
        CHECK(vm.makeBool(true) == vm.makeBool(true));
        CHECK(!(vm.makeBool(true) == vm.makeBool(false)));
    }

    return RUN_TESTS();
}
