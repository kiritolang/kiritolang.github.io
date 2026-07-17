// embed_event_sourcing.cpp — an event-sourced aggregate with snapshot serialization. C++ owns the
// event log (a List of event Dicts) and the replay loop; Kirito owns the reducer
// Function(state: Dict, event: Dict) -> Dict that folds one event onto the running state (deposit/
// withdraw/…). C++ replays the whole log from a seed state to rebuild the aggregate, then Kirito
// serializes the rebuilt snapshot with dump.dumps / dump.loads and asserts the round trip is
// value-identical. Replaying twice must yield equal state (determinism); an unknown event type must
// make the reducer throw.
//
// Flow: C++ (seed state + event log) → for each event → Kirito (reducer folds it in) → C++ (final
// state) → Kirito (dump serialize/deserialize) → C++ (equals check).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// One domain event the C++ log holds before it is handed to Kirito as a Dict.
struct AccountEvent {
    std::string type;    // "deposit" / "withdraw" / "fee" / …
    int64_t     amount;
};

static Handle eventToDict(KiritoVM& vm, const AccountEvent& e) {
    Dict d(vm);
    d.set("type",   Value(vm, e.type));
    d.set("amount", Value(vm, e.amount));
    return d.handle();
}

// An event-sourced bank account: C++ owns the log, Kirito owns the fold.
class Aggregate {
public:
    Aggregate(KiritoVM& vm, Handle reducer) : vm_(vm), reducer_(reducer) {}

    void append(const AccountEvent& e) { log_.push_back(e); }

    // Replay the whole log from a fresh seed state, folding each event through the Kirito reducer.
    // Returns a handle to the rebuilt state Dict, GC-rooted by the caller's RootScope.
    Handle replay(RootScope& rs) const {
        Dict seed(vm_);
        seed.set("balance", Value(vm_, int64_t{0}));
        seed.set("count",   Value(vm_, int64_t{0}));
        Handle stateH = rs.add(seed.handle());
        for (const AccountEvent& e : log_) {
            Handle evH = rs.add(eventToDict(vm_, e));
            std::array<Handle, 2> args{stateH, evH};
            Handle nextH = rs.add(vm_.arena().deref(reducer_).call(vm_, args));
            Value next(vm_, nextH);
            // The reducer is contracted to return the new state Dict; anything else is a bug.
            if (!next.isDict())
                throw KiritoError("aggregate: reducer must return a Dict, got '" + next.typeName() + "'");
            stateH = nextH;
        }
        return stateH;
    }

    std::size_t logSize() const { return log_.size(); }

private:
    KiritoVM&          vm_;
    Handle             reducer_;
    std::vector<AccountEvent> log_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // The pluggable reducer: fold one event onto the state. deposit adds, withdraw/fee subtract,
    // and every event bumps a running count. An unknown type throws — a genuine domain error.
    Handle reducer = compile(R"KI(
Function(state, event) -> Dict:
    var kind = event["type"]
    var amt = event["amount"]
    var bal = state["balance"]
    if kind == "deposit":
        bal = bal + amt
    elif kind == "withdraw":
        bal = bal - amt
    elif kind == "fee":
        bal = bal - amt
    else:
        throw "unknown event type: " + kind
    return {"balance": bal, "count": state["count"] + 1}
)KI");

    Aggregate acct(vm, reducer);
    // A scripted event sequence: +100, +50, -30, -5(fee)  ->  balance 115, count 4.
    acct.append({"deposit",  100});
    acct.append({"deposit",  50});
    acct.append({"withdraw", 30});
    acct.append({"fee",      5});

    RootScope rs(vm);
    Handle rebuiltH = acct.replay(rs);
    Value rebuilt(vm, rebuiltH);

    // ---- scripted-sequence assertions ----
    CHECK(acct.logSize() == 4);
    CHECK(rebuilt.isDict());
    CHECK(rebuilt.get("balance").asInt("balance") == 115);
    CHECK(rebuilt.get("count").asInt("count") == 4);

    // ---- replay determinism: replaying the same log again yields an equal state ----
    {
        RootScope rs2(vm);
        Handle againH = acct.replay(rs2);
        Value again(vm, againH);
        CHECK(again.get("balance").asInt("balance") == 115);
        CHECK(rebuilt.equals(again));
    }

    // ---- persistence: serialize the rebuilt snapshot with dump, reconstruct, assert equal ----
    // The reducer's result Dict is handed to a Kirito round-trip function that dumps it to Bytes and
    // loads it back. C++ verifies the blob is Bytes and the reconstructed Dict equals the original.
    {
        Handle roundtripH = compile(R"KI(
Function(state) -> Dict:
    var mod = import("dump")
    var blob = mod.dumps(state)
    return {"blob": blob, "restored": mod.loads(blob)}
)KI");
        Value roundtrip(vm, roundtripH);
        Value out = roundtrip.call({rebuilt});
        Value blob     = out.get("blob");
        Value restored = out.get("restored");

        CHECK(blob.isBytes());          // dump.dumps produces raw Bytes
        CHECK(restored.isDict());
        CHECK(restored.equals(rebuilt));  // value-identical after the round trip
        CHECK(restored.get("balance").asInt("balance") == 115);
        CHECK(restored.get("count").asInt("count") == 4);
    }

    // ---- overdraft is allowed (no policy against it) — pure fold, negative balance is fine ----
    {
        Aggregate od(vm, reducer);
        od.append({"deposit",  10});
        od.append({"withdraw", 25});
        RootScope rs3(vm);
        Value s(vm, od.replay(rs3));
        CHECK(s.get("balance").asInt("balance") == -15);
        CHECK(s.get("count").asInt("count") == 2);
    }

    // ---- adversarial: an unknown event type makes the reducer throw during replay ----
    {
        Aggregate bad(vm, reducer);
        bad.append({"deposit", 100});
        bad.append({"teleport", 1});   // no such event — reducer throws
        CHECK_THROWS([&] {
            RootScope rsBad(vm);
            bad.replay(rsBad);
        }());
    }

    return RUN_TESTS();
}
