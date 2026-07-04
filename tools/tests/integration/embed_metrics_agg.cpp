// embed_metrics_agg.cpp — a streaming metrics aggregator. C++ owns the sample stream and the
// collected results; Kirito owns the pluggable policy: a reducer that folds each sample into a
// per-metric running state (count/sum/min/max), a summarizer that turns the folded state into
// per-metric summaries (mean etc.), and an alert predicate that flags a summary over a threshold.
//
// Flow: C++ (stream of {name, value} Dicts) → Kirito reducer (fold, one sample at a time) →
// Kirito summarizer (state → List of summary Dicts) → Kirito alert (summary → Bool) →
// C++ collects the flagged metric names.

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

struct Sample {
    std::string name;
    double      value;
};

// A metrics aggregator: C++ drives the loop, Kirito supplies reducer/summarizer/alert.
class Aggregator {
public:
    Aggregator(KiritoVM& vm, Handle reducer, Handle summarizer, Handle alert)
        : vm_(vm), reducer_(reducer), summarizer_(summarizer), alert_(alert) {
        state_ = Value(vm_, Dict(vm_).handle());
    }

    // Fold one sample into the running state via the Kirito reducer.
    void ingest(const Sample& s) {
        Dict sample(vm_);
        sample.set("name",  Value(vm_, s.name));
        sample.set("value", Value(vm_, s.value));
        Value fn(vm_, reducer_);
        Value next = fn.call({ state_, Value(vm_, sample.handle()) });
        // The reducer MUST return a Dict — the whole running state. Fail loudly otherwise.
        if (!next.isDict())
            throw KiritoError("aggregator: reducer must return a Dict, got '" + next.typeName() + "'");
        state_ = next;
    }

    // Summarize the folded state → a List of summary Dicts (one per metric).
    std::vector<Value> summaries() {
        Value fn(vm_, summarizer_);
        Value out = fn.call({ state_ });
        if (!out.isList())
            throw KiritoError("aggregator: summarizer must return a List, got '" + out.typeName() + "'");
        return out.items();
    }

    // Run the alert predicate over every summary; collect the flagged metric names in order.
    std::vector<std::string> flagged() {
        std::vector<std::string> names;
        Value fn(vm_, alert_);
        for (Value summary : summaries()) {
            Value hot = fn.call({ summary });
            if (!hot.isBool())
                throw KiritoError("aggregator: alert must return a Bool, got '" + hot.typeName() + "'");
            if (hot.truthy())
                names.push_back(summary.get("name").asStringRef("name"));
        }
        return names;
    }

    Value state() const { return state_; }

private:
    KiritoVM& vm_;
    Handle    reducer_;
    Handle    summarizer_;
    Handle    alert_;
    Value     state_{Value::None(vm_)};
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // Reducer: fold a {name, value} sample into per-metric running state. Each metric maps to a
    // Dict {count, sum, min, max}. A metric seen for the first time is initialized.
    Handle reducer = compile(R"KI(
Function(state : Dict, sample : Dict) -> Dict:
    var name = sample["name"]
    var v = sample["value"]
    if name in state:
        var cur = state[name]
        cur["count"] = cur["count"] + 1
        cur["sum"] = cur["sum"] + v
        cur["min"] = v if v < cur["min"] else cur["min"]
        cur["max"] = v if v > cur["max"] else cur["max"]
        state[name] = cur
    else:
        state[name] = {"count": 1, "sum": v, "min": v, "max": v}
    return state
)KI");

    // Summarizer: state Dict → List of per-metric summary Dicts, computing the mean. Uses the
    // statistics module's mean over the reconstructed-ish values where possible; here mean is
    // sum/count directly (statistics is imported to prove module access works across the boundary).
    Handle summarizer = compile(R"KI(
Function(state : Dict) -> List:
    var stats = import("statistics")
    var out = []
    for name in state:
        var m = state[name]
        var mean = m["sum"] / m["count"]
        # cross-check with statistics.mean on a two-point [min, max] set is meaningless here;
        # instead confirm the module loads and use it on the trivial single-value list.
        var checkmean = stats.mean([m["sum"] / m["count"]])
        out.append({"name": name, "count": m["count"], "mean": mean,
                    "min": m["min"], "max": m["max"], "range": m["max"] - m["min"],
                    "checkmean": checkmean})
    return out
)KI");

    // Alert: flag a metric whose mean exceeds a threshold (80.0).
    Handle alert = compile(R"KI(
Function(summary : Dict) -> Bool:
    return summary["mean"] > 80.0
)KI");

    Aggregator agg(vm, reducer, summarizer, alert);

    // ~10 samples across 3 metric names.
    const std::vector<Sample> stream = {
        {"cpu",    50.0},
        {"cpu",    90.0},
        {"cpu",    70.0},
        {"cpu",    100.0},
        {"mem",    30.0},
        {"mem",    40.0},
        {"mem",    50.0},
        {"lat",    120.0},
        {"lat",    80.0},
        {"lat",    100.0},
    };
    for (const Sample& s : stream)
        agg.ingest(s);

    // ---- verify the folded state ----
    Value state = agg.state();
    CHECK(state.isDict());
    CHECK(state.len() == 3);

    Value cpu = state.get("cpu");
    CHECK(cpu.get("count").asInt("count") == 4);
    // cpu mean = (50+90+70+100)/4 = 77.5
    CHECK(Float(vm, cpu.get("sum").asFloat("sum")).compare(Value(vm, 310.0)));
    CHECK(cpu.get("min").asFloat("min") == 50.0);
    CHECK(cpu.get("max").asFloat("max") == 100.0);

    // ---- verify the summaries (mean etc.) ----
    std::vector<Value> sums = agg.summaries();
    CHECK(sums.size() == 3);

    // Pull the summaries into a name→mean lookup for order-independent checks.
    auto meanOf = [&](const std::string& metric) -> double {
        for (Value s : sums)
            if (s.get("name").asStringRef("name") == metric)
                return s.get("mean").asFloat("mean");
        throw KiritoError("no summary for metric '" + metric + "'");
    };
    // cpu mean 77.5, mem mean 40.0, lat mean 100.0
    CHECK(Float(vm, meanOf("cpu")).compare(Value(vm, 77.5)));
    CHECK(Float(vm, meanOf("mem")).compare(Value(vm, 40.0)));
    CHECK(Float(vm, meanOf("lat")).compare(Value(vm, 100.0)));

    // The statistics.mean cross-check equals the mean itself.
    for (Value s : sums)
        CHECK(Float(vm, s.get("mean").asFloat("mean"))
                  .compare(Value(vm, s.get("checkmean").asFloat("checkmean"))));

    // ---- verify which metrics alerted (mean > 80.0): only "lat" (100.0). ----
    std::vector<std::string> hot = agg.flagged();
    CHECK(hot.size() == 1);
    CHECK(hot.at(0) == "lat");

    // ---- an empty stream summarizes to nothing and flags nothing ----
    {
        Aggregator empty(vm, reducer, summarizer, alert);
        CHECK(empty.summaries().empty());
        CHECK(empty.flagged().empty());
    }

    // ---- lowering the alert threshold flags more metrics; here a strict reducer whose alert
    //      returns non-Bool is the adversarial case (below). First a valid multi-flag run. ----
    {
        Handle alertLow = compile(R"KI(
Function(summary : Dict) -> Bool:
    return summary["mean"] >= 50.0
)KI");
        Aggregator agg2(vm, reducer, summarizer, alertLow);
        for (const Sample& s : stream)
            agg2.ingest(s);
        std::vector<std::string> hot2 = agg2.flagged();
        // cpu (77.5) and lat (100.0) are >= 50; mem (40.0) is not.
        CHECK(hot2.size() == 2);
        bool hasCpu = false, hasLat = false;
        for (const std::string& n : hot2) {
            if (n == "cpu") hasCpu = true;
            if (n == "lat") hasLat = true;
        }
        CHECK(hasCpu);
        CHECK(hasLat);
    }

    // ---- adversarial: a reducer that returns a non-Dict must throw ----
    {
        Handle badReducer = compile(R"KI(
Function(state : Dict, sample : Dict):
    return "not-a-dict"
)KI");
        Aggregator broken(vm, badReducer, summarizer, alert);
        CHECK_THROWS(broken.ingest({"cpu", 1.0}));
    }

    // ---- adversarial: a reducer indexing a missing sample key throws (bad key) ----
    {
        Handle keyReducer = compile(R"KI(
Function(state : Dict, sample : Dict) -> Dict:
    var bogus = sample["missing"]
    return state
)KI");
        Aggregator broken(vm, keyReducer, summarizer, alert);
        CHECK_THROWS(broken.ingest({"cpu", 1.0}));
    }

    // ---- adversarial: an alert predicate returning a non-Bool must throw ----
    {
        Handle badAlert = compile(R"KI(
Function(summary : Dict):
    return summary["mean"]
)KI");
        Aggregator broken(vm, reducer, summarizer, badAlert);
        broken.ingest({"cpu", 90.0});
        CHECK_THROWS(broken.flagged());
    }

    return RUN_TESTS();
}
