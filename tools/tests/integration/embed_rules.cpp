// embed_rules.cpp — a small forward-chaining rules engine. C++ owns the fact base (a working
// memory of Fact records) and the schedule; each RULE is a Kirito Dict:
//
//     {"name": String,
//      "match": Function(kb : Dict) -> List,     // list of "binding" dicts; empty = no match
//      "fire":  Function(kb : Dict, b : Dict) -> List}   // returns any NEW facts to assert
//
// The engine loops until nothing new was derived (fixpoint), then verifies the closure.
// Classic zoo example: from `has(x, "feathers")` conclude `is(x, "bird")`, from `is(x, "bird")`
// AND `has(x, "wings")` AND `swims(x)` conclude `is(x, "penguin")`, etc.

#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// A fact is a 3-tuple (predicate, arg1, arg2). arg2 is empty for unary preds like `is(x, y)`.
struct Fact {
    std::string pred, a, b;
    bool operator==(const Fact& o) const { return pred == o.pred && a == o.a && b == o.b; }
};

struct FactHash {
    std::size_t operator()(const Fact& f) const {
        auto h = std::hash<std::string>{};
        return h(f.pred) ^ (h(f.a) << 1) ^ (h(f.b) << 2);
    }
};

// Build the "kb" (knowledge base) Dict a Kirito rule sees: {pred: List of [a, b] pairs}.
static Handle buildKb(KiritoVM& vm, const std::vector<Fact>& facts) {
    // Bucket facts by predicate for fast Kirito-side iteration.
    std::unordered_map<std::string, std::vector<const Fact*>> byPred;
    for (const auto& f : facts) byPred[f.pred].push_back(&f);
    Dict d(vm);
    for (const auto& [pred, list] : byPred) {
        List row(vm);
        for (const Fact* f : list) {
            List pair(vm);
            pair.add(Value(vm, f->a));
            pair.add(Value(vm, f->b));
            row.add(pair.build());
        }
        d.set(pred, row.build());
    }
    return d.build().handle();
}

// Iterate a Value known to be a List of Facts (each Fact is a 3-element list [pred, a, b])
// and append them to `facts` if not already present. Returns the number of NEW facts.
static int64_t assertNew(std::vector<Fact>& facts, std::unordered_set<Fact, FactHash>& seen,
                         Value returned) {
    int64_t added = 0;
    for (Value item : returned.items()) {
        auto parts = item.items();
        if (parts.size() < 2 || parts.size() > 3)
            throw KiritoError("rule fire must yield [pred, a] or [pred, a, b] triples");
        Fact f{parts[0].asStringRef("pred"), parts[1].asStringRef("a"),
               parts.size() == 3 ? parts[2].asStringRef("b") : std::string{}};
        if (seen.insert(f).second) {
            facts.push_back(f);
            ++added;
        }
    }
    return added;
}

// Drive rules to fixpoint. Returns the number of derivation rounds performed.
static int64_t runToFixpoint(KiritoVM& vm, std::vector<Handle>& rules, std::vector<Fact>& facts) {
    std::unordered_set<Fact, FactHash> seen(facts.begin(), facts.end());
    int64_t rounds = 0;
    while (true) {
        int64_t addedThisRound = 0;
        for (Handle rH : rules) {
            // Rebuild the kb Dict BEFORE each rule so a rule fired earlier in the same round is
            // visible to a later rule (a forward-chainer that "iterates fastest wins" without
            // needing multiple fixpoint sweeps — classic RETE-lite).
            Handle kbH = buildKb(vm, facts);
            RootScope rs(vm);
            Handle kbR = rs.add(kbH);
            Value rV(vm, rH);
            Value matchFn = rV.get("match");
            Value fireFn  = rV.get("fire");
            std::array<Handle, 1> matchArgs{kbR};
            Handle bindingsH = rs.add(vm.arena().deref(matchFn).call(vm, matchArgs));
            Value bindings(vm, bindingsH);
            for (Value b : bindings.items()) {
                std::array<Handle, 2> fireArgs{kbR, b.handle()};
                Handle newFactsH = rs.add(vm.arena().deref(fireFn).call(vm, fireArgs));
                addedThisRound += assertNew(facts, seen, Value(vm, newFactsH));
            }
        }
        ++rounds;
        if (addedThisRound == 0) break;
        if (rounds > 100) throw KiritoError("rules: exceeded 100 rounds without fixpoint");
    }
    return rounds;
}

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // Rule 1: has(X, "feathers") -> is(X, "bird")
    Handle r_bird = compile(R"KI(
var m = Function(kb) -> List:
    var out = []
    if "has" in kb:
        for pair in kb["has"]:
            if pair[1] == "feathers":
                out.append({"x": pair[0]})
    return out
var f = Function(kb, b) -> List:
    return [["is", b["x"], "bird"]]
{"name": "featherToBird", "match": m, "fire": f}
)KI");

    // Rule 2: is(X, "bird") AND swims(X) -> is(X, "penguin")
    Handle r_penguin = compile(R"KI(
var m = Function(kb) -> List:
    var out = []
    if "is" not in kb:
        return out
    if "swims" not in kb:
        return out
    for iPair in kb["is"]:
        if iPair[1] == "bird":
            for sPair in kb["swims"]:
                if sPair[0] == iPair[0]:
                    out.append({"x": iPair[0]})
    return out
var f = Function(kb, b) -> List:
    return [["is", b["x"], "penguin"]]
{"name": "swimmingBird", "match": m, "fire": f}
)KI");

    // Rule 3: is(X, "bird") -> can(X, "fly") — but is(X, "penguin") -> NOT can(X, "fly")
    Handle r_canFly = compile(R"KI(
var m = Function(kb) -> List:
    var out = []
    if "is" not in kb:
        return out
    var penguins = Set()
    for pair in kb["is"]:
        if pair[1] == "penguin":
            penguins.add(pair[0])
    for pair in kb["is"]:
        if pair[1] == "bird" and pair[0] not in penguins:
            out.append({"x": pair[0]})
    return out
var f = Function(kb, b) -> List:
    return [["can", b["x"], "fly"]]
{"name": "birdsCanFly", "match": m, "fire": f}
)KI");

    // Rule 4: is(X, "penguin") -> is(X, "waterfowl") (transitive classification)
    Handle r_water = compile(R"KI(
var m = Function(kb) -> List:
    var out = []
    if "is" not in kb:
        return out
    for pair in kb["is"]:
        if pair[1] == "penguin":
            out.append({"x": pair[0]})
    return out
var f = Function(kb, b) -> List:
    return [["is", b["x"], "waterfowl"]]
{"name": "penguinIsWaterfowl", "match": m, "fire": f}
)KI");

    // Order matters: bird-classify first (so is-facts exist), then penguin-classify (so the
    // canFly rule can see the penguin and skip it), then canFly, then waterfowl.
    std::vector<Handle> rules{r_bird, r_penguin, r_canFly, r_water};
    // Initial facts: pingu has feathers, wings, and swims; robin has feathers.
    std::vector<Fact> facts;
    facts.push_back({"has",   "pingu", "feathers"});
    facts.push_back({"has",   "pingu", "wings"});
    facts.push_back({"swims", "pingu", ""});
    facts.push_back({"has",   "robin", "feathers"});

    int64_t rounds = runToFixpoint(vm, rules, facts);
    CHECK(rounds >= 2);
    CHECK(rounds <= 6);

    auto hasFact = [&](Fact f) {
        for (const auto& x : facts) if (x == f) return true;
        return false;
    };
    CHECK(hasFact({"is", "pingu", "bird"}));
    CHECK(hasFact({"is", "pingu", "penguin"}));
    CHECK(hasFact({"is", "pingu", "waterfowl"}));
    CHECK(!hasFact({"can", "pingu", "fly"}));      // penguins do NOT fly
    CHECK(hasFact({"is", "robin", "bird"}));
    CHECK(hasFact({"can", "robin", "fly"}));       // robin is not a penguin -> can fly

    // ---- idempotence: running again on the closed set produces no new facts ----
    {
        auto before = facts.size();
        int64_t rounds2 = runToFixpoint(vm, rules, facts);
        CHECK(facts.size() == before);
        CHECK(rounds2 == 1);
    }

    // ---- adversarial: a rule whose `fire` returns a badly-shaped triple throws ----
    {
        Handle badFire = compile(R"KI(
var m = Function(kb) -> List: return [{"x": "n/a"}]
var f = Function(kb, b) -> List: return [["only-one-arg"]]
{"match": m, "fire": f}
)KI");
        std::vector<Handle> r2{badFire};
        std::vector<Fact> f2;
        CHECK_THROWS(runToFixpoint(vm, r2, f2));
    }

    // ---- infinite-rule detection: a rule that always finds a new binding is capped at 100
    //      rounds so a bad ruleset can't hang the engine ----
    {
        Handle inf = compile(R"KI(
var counter = [0]
var m = Function(kb) -> List: return [{"n": counter[0]}]
var f = Function(kb, b):
    counter[0] = counter[0] + 1
    return [["counted", String(counter[0] - 1), ""]]
{"match": m, "fire": f}
)KI");
        std::vector<Handle> rInf{inf};
        std::vector<Fact> fInf;
        CHECK_THROWS(runToFixpoint(vm, rInf, fInf));
        CHECK(fInf.size() >= 100);   // it stopped just past the cap
    }

    return RUN_TESTS();
}
