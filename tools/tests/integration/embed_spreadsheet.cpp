// embed_spreadsheet.cpp — a tiny spreadsheet/formula engine. C++ owns the grid of cells
// (name → numeric Value) and the evaluation order; Kirito owns each formula: a
// Function(cells: Dict) -> (Integer or Float) that computes a cell's value from a snapshot Dict of
// the already-evaluated cells (e.g. C = Function(cells): return cells["A"] + cells["B"] * 2).
//
// Flow per recalc: C++ walks the cells in dependency (topological) order → for each formula cell,
// builds a Dict snapshot of the values computed so far → Kirito (the formula) → C++ validates the
// result is numeric and stores it back into the grid.

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// A cell is either a literal number or a formula (a compiled Kirito function). C++ keeps the last
// computed numeric value alongside so the snapshot Dict can be built cheaply.
struct Cell {
    bool                hasFormula = false;
    Handle              formula{};        // valid iff hasFormula
    Value               value;            // last computed value (literal seed, then result)
    explicit Cell(Value v) : value(v) {}
    Cell(Handle fn, Value seed) : hasFormula(true), formula(fn), value(seed) {}
};

class Sheet {
public:
    explicit Sheet(KiritoVM& vm) : vm_(vm) {}

    // A literal cell: a fixed number (Integer or Float).
    void put(const std::string& name, Value v) {
        if (!v.isNumber())
            throw KiritoError("sheet: literal '" + name + "' must be numeric");
        cells_.emplace(name, Cell(v));
    }
    // A formula cell: a Kirito Function(cells: Dict) -> number. `seed` is its pre-recalc placeholder.
    void putFormula(const std::string& name, Handle fn) {
        cells_.emplace(name, Cell(fn, Value(vm_, 0)));
    }

    // Recompute every formula cell, visiting names in the given dependency (topological) order so a
    // formula only ever reads cells already evaluated this pass.
    void recalc(const std::vector<std::string>& order) {
        RootScope rs(vm_);
        for (const std::string& name : order) {
            auto it = cells_.find(name);
            if (it == cells_.end())
                throw KiritoError("sheet: unknown cell '" + name + "' in recalc order");
            Cell& c = it->second;
            if (!c.hasFormula) continue;

            // Snapshot the values computed so far (literals + earlier formulas this pass).
            Dict snap(vm_);
            for (const auto& [n, cell] : cells_)
                snap.set(n, cell.value);

            std::array<Handle, 1> args{snap.handle()};
            Handle rH = rs.add(vm_.arena().deref(c.formula).call(vm_, args));
            Value result(vm_, rH);
            // The formula MUST hand back a number — anything else is a broken sheet. Detect in C++.
            if (!result.isNumber())
                throw KiritoError("sheet: formula '" + name + "' returned non-numeric '" +
                                  result.typeName() + "'");
            c.value = result;
        }
    }

    Value get(const std::string& name) const {
        auto it = cells_.find(name);
        if (it == cells_.end())
            throw KiritoError("sheet: no cell '" + name + "'");
        return it->second.value;
    }

private:
    KiritoVM&                             vm_;
    std::unordered_map<std::string, Cell> cells_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // ---- a real little sheet ----------------------------------------------------------------
    // Literals:  A = 3 (Integer), B = 4 (Integer), RATE = 1.5 (Float)
    // Formulas:  C = A + B * 2                       -> 11  (Integer)
    //            D = C / B                            -> 2.75 (Float — true division)
    //            E = (A + B) * RATE                   -> 10.5 (Float — Int*Float)
    //            TOTAL = C + D + E                    -> 24.25 (Float)
    Sheet sheet(vm);
    sheet.put("A", Value(vm, 3));
    sheet.put("B", Value(vm, 4));
    sheet.put("RATE", Value(vm, 1.5));

    sheet.putFormula("C", compile(R"KI(
Function(cells):
    return cells["A"] + cells["B"] * 2
)KI"));
    sheet.putFormula("D", compile(R"KI(
Function(cells):
    return cells["C"] / cells["B"]
)KI"));
    sheet.putFormula("E", compile(R"KI(
Function(cells):
    return (cells["A"] + cells["B"]) * cells["RATE"]
)KI"));
    sheet.putFormula("TOTAL", compile(R"KI(
Function(cells):
    return cells["C"] + cells["D"] + cells["E"]
)KI"));

    // Topological order: each formula only reads cells evaluated before it.
    std::vector<std::string> order{"C", "D", "E", "TOTAL"};
    sheet.recalc(order);

    // ---- verifications ----------------------------------------------------------------------
    // C = 3 + 4*2 = 11, still an Integer (Int arithmetic stays Int).
    CHECK(sheet.get("C").isInt());
    CHECK(sheet.get("C").asInt("C") == 11);

    // D = 11 / 4 = 2.75 — true division makes it a Float.
    CHECK(sheet.get("D").isFloat());
    CHECK(Float(vm, sheet.get("D").asFloat("D")).compare(Value(vm, 2.75)));

    // E = (3 + 4) * 1.5 = 10.5 — Int * Float promotes to Float.
    CHECK(sheet.get("E").isFloat());
    CHECK(Float(vm, sheet.get("E").asFloat("E")).compare(Value(vm, 10.5)));

    // TOTAL = 11 + 2.75 + 10.5 = 24.25 (Float).
    CHECK(sheet.get("TOTAL").isFloat());
    CHECK(Float(vm, sheet.get("TOTAL").asFloat("TOTAL")).compare(Value(vm, 24.25)));

    // Literals are untouched by recalc.
    CHECK(sheet.get("A").asInt("A") == 3);
    CHECK(sheet.get("RATE").asFloat("RATE") == 1.5);

    // ---- recalc is idempotent: running it again yields the same numbers ----------------------
    sheet.recalc(order);
    CHECK(sheet.get("C").asInt("C") == 11);
    CHECK(Float(vm, sheet.get("TOTAL").asFloat("TOTAL")).compare(Value(vm, 24.25)));

    // ---- a fresh sheet responds to changed inputs -------------------------------------------
    {
        Sheet s2(vm);
        s2.put("X", Value(vm, 10));
        s2.put("Y", Value(vm, 2));
        s2.putFormula("HALF", compile(R"KI(
Function(cells):
    return cells["X"] / cells["Y"]
)KI"));
        s2.recalc({"HALF"});
        // 10 / 2 = 5.0 — true division, a whole-number Float, not Integer 5.
        CHECK(s2.get("HALF").isFloat());
        CHECK(s2.get("HALF").asFloat("HALF") == 5.0);
    }

    // ---- adversarial: a formula that references a MISSING cell key throws --------------------
    {
        Sheet bad(vm);
        bad.put("A", Value(vm, 1));
        bad.putFormula("Z", compile(R"KI(
Function(cells):
    return cells["A"] + cells["NOPE"]
)KI"));
        CHECK_THROWS(bad.recalc({"Z"}));
    }

    // ---- adversarial: a formula returning a String is caught by C++ (isNumber() false) -------
    {
        Sheet bad(vm);
        bad.put("A", Value(vm, 1));
        bad.putFormula("S", compile(R"KI(
Function(cells):
    return "not a number"
)KI"));
        CHECK_THROWS(bad.recalc({"S"}));
    }

    return RUN_TESTS();
}
