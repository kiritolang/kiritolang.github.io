// embed_query_engine.cpp — an in-memory query engine over a table of row Dicts. C++ owns the table
// (a vector<Handle> of GC-rooted row Dicts) and the filter→project→reduce control loop; Kirito owns
// the query LOGIC: a WHERE predicate Function(row: Dict) -> Bool, a projection
// Function(row: Dict) -> Dict, and an aggregation reducer Function(acc, row: Dict) -> <acc> with a
// C++-supplied seed.
//
// Flow per query: C++ (iterate table) → Kirito (predicate) → C++ (keep/drop) → Kirito (projection or
// reducer) → C++ (collect / accumulate result).

#include <cstdint>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// A person row in the source domain. C++ builds each into a Kirito Dict the queries operate on.
struct Person {
    std::string name;
    int64_t     age;
    std::string dept;
};

class QueryEngine {
public:
    explicit QueryEngine(KiritoVM& vm) : vm_(vm) {}

    // Build a row Dict and keep its Handle rooted for the engine's lifetime (the table outlives any
    // single query call, so a bare RootScope per-load would sweep it).
    void load(const Person& p) {
        Dict d(vm_);
        d.set("name", Value(vm_, p.name));
        d.set("age",  Value(vm_, p.age));
        d.set("dept", Value(vm_, p.dept));
        rows_.push_back(roots_.add(d.handle()));
    }

    std::size_t size() const { return rows_.size(); }

    // WHERE: run the predicate over every row, return the rows for which it is truthy. A predicate
    // that returns a non-Bool is a programming error in the query — reject it loudly.
    std::vector<Value> filter(Handle predicate) const {
        std::vector<Value> kept;
        for (Handle rowH : rows_) {
            std::array<Handle, 1> args{rowH};
            Value verdict(vm_, vm_.arena().deref(predicate).call(vm_, args));
            if (!verdict.isBool())
                throw KiritoError("query: WHERE predicate must return a Bool, got '" +
                                  verdict.typeName() + "'");
            if (verdict.asBool("predicate"))
                kept.push_back(Value(vm_, rowH));
        }
        return kept;
    }

    // PROJECT: map each row through a projection returning a new Dict (a subset/rename of columns).
    std::vector<Value> project(const std::vector<Value>& rowsIn, Handle projection) const {
        std::vector<Value> out;
        for (const Value& row : rowsIn) {
            std::array<Handle, 1> args{row.handle()};
            Value projected(vm_, vm_.arena().deref(projection).call(vm_, args));
            if (!projected.isDict())
                throw KiritoError("query: projection must return a Dict, got '" +
                                  projected.typeName() + "'");
            out.push_back(projected);
        }
        return out;
    }

    // REDUCE: fold the rows with Function(acc, row) -> acc, threading the accumulator from `seed`.
    Value reduce(const std::vector<Value>& rowsIn, Value seed, Handle reducer) const {
        Value acc = seed;
        for (const Value& row : rowsIn) {
            std::array<Handle, 2> args{acc.handle(), row.handle()};
            acc = Value(vm_, vm_.arena().deref(reducer).call(vm_, args));
        }
        return acc;
    }

private:
    KiritoVM&           vm_;
    RootScope           roots_{vm_};   // keeps every loaded row Dict alive for the engine's life
    std::vector<Handle> rows_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    QueryEngine eng(vm);
    eng.load({"Alice",   34, "eng"});
    eng.load({"Bob",     29, "eng"});
    eng.load({"Carol",   41, "sales"});
    eng.load({"Dave",    23, "sales"});
    eng.load({"Erin",    30, "eng"});
    eng.load({"Frank",   55, "ops"});
    CHECK(eng.size() == 6);

    // ---- (a) WHERE age >= 30, then COUNT ----
    Handle adults = compile(R"KI(
Function(row) -> Bool:
    return row["age"] >= 30
)KI");
    std::vector<Value> grown = eng.filter(adults);
    // Alice(34), Carol(41), Erin(30), Frank(55)
    CHECK(grown.size() == 4);

    // ---- (b) PROJECT name + dept off the filtered rows ----
    Handle nameDept = compile(R"KI(
Function(row) -> Dict:
    return {"name": row["name"], "dept": row["dept"]}
)KI");
    std::vector<Value> cards = eng.project(grown, nameDept);
    CHECK(cards.size() == 4);
    // Each projection has exactly the two requested columns and no "age".
    for (Value c : cards) {
        CHECK(c.isDict());
        CHECK(c.len() == 2);
        CHECK(c.contains(Value(vm, "name")));
        CHECK(c.contains(Value(vm, "dept")));
        CHECK(!c.contains(Value(vm, "age")));
    }
    // First filtered row is Alice/eng (table order is preserved through filter+project).
    CHECK(cards.front().get("name").asStringRef("name") == "Alice");
    CHECK(cards.front().get("dept").asStringRef("dept") == "eng");

    // ---- (c) REDUCE: sum of ages over the adults via a Kirito reducer + C++ seed ----
    Handle sumAges = compile(R"KI(
Function(acc, row) -> Integer:
    return acc + row["age"]
)KI");
    Value total = eng.reduce(grown, Value(vm, int64_t{0}), sumAges);
    CHECK(total.isInt());
    // 34 + 41 + 30 + 55 = 160
    CHECK(total.asInt("total") == 160);

    // Reduce over ALL rows (no filter) for a whole-table check: 34+29+41+23+30+55 = 212.
    std::vector<Value> everyone;
    {
        // A pass-through predicate to materialize the full table as a row vector.
        Handle keepAll = compile("Function(row) -> Bool:\n    return True\n");
        everyone = eng.filter(keepAll);
    }
    CHECK(everyone.size() == 6);
    Value grandTotal = eng.reduce(everyone, Value(vm, int64_t{0}), sumAges);
    CHECK(grandTotal.asInt("grandTotal") == 212);

    // A reducer that builds a per-dept count Dict proves the accumulator can be any value, not just a
    // scalar — Kirito owns the shape of the fold.
    Handle deptCounts = compile(R"KI(
Function(acc, row) -> Dict:
    var d = row["dept"]
    acc[d] = acc.get(d, 0) + 1
    return acc
)KI");
    Value counts = eng.reduce(everyone, Value(vm, Dict(vm).handle()), deptCounts);
    CHECK(counts.isDict());
    CHECK(counts.get("eng").asInt("eng") == 3);
    CHECK(counts.get("sales").asInt("sales") == 2);
    CHECK(counts.get("ops").asInt("ops") == 1);

    // ---- adversarial: a WHERE predicate that returns a non-Bool must throw ----
    {
        Handle badPred = compile(R"KI(
Function(row):
    return row["age"]
)KI");
        CHECK_THROWS(eng.filter(badPred));
    }

    // ---- adversarial: a projection that returns a non-Dict must throw ----
    {
        Handle badProj = compile("Function(row):\n    return row[\"name\"]\n");
        CHECK_THROWS(eng.project(everyone, badProj));
    }

    // ---- adversarial: a predicate that reads a missing column throws a KeyError from Kirito ----
    {
        Handle missing = compile(R"KI(
Function(row) -> Bool:
    return row["salary"] > 0
)KI");
        CHECK_THROWS(eng.filter(missing));
    }

    return RUN_TESTS();
}
