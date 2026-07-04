// embed_fsm.cpp — a configurable finite state machine (a coin-operated turnstile). C++ owns the
// current state (a String) and a context Dict (counters/coins); Kirito owns the transition policy —
// each transition is a Function(state: String, event: Dict, ctx: Dict) -> Dict returning
// {"state": <next state String>, "ctx": <updated ctx Dict>}.
//
// Flow per event: C++ (feed event) → Kirito (guard + transition decision) → C++ (adopt the returned
// state + ctx, validating the shape). A guard that rejects an illegal event returns the SAME state.

#include <array>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// The machine: C++ owns `state` (a String) and `ctx` (a Dict). Each `step(event)` hands
// (state, event, ctx) to the Kirito transition function and adopts the {"state", "ctx"} it returns.
class StateMachine {
public:
    StateMachine(KiritoVM& vm, Handle transition, const std::string& start)
        : vm_(vm), transition_(transition), state_(start), ctx_(vm) {}

    // Feed one event Dict. Returns nothing; state_/ctx_ are updated in place. Throws (via
    // KiritoError) if the transition returns something that is not a well-formed result Dict.
    void step(Value event) {
        RootScope rs(vm_);
        Handle stateH = rs.add(Value(vm_, state_).handle());
        Handle ctxH   = rs.add(ctx_.handle());
        std::array<Handle, 3> args{stateH, event.handle(), ctxH};
        Handle resultH = rs.add(vm_.arena().deref(transition_).call(vm_, args));
        Value result(vm_, resultH);

        // The transition MUST return a Dict; anything else is a policy bug — fail loudly.
        if (!result.isDict())
            throw KiritoError("fsm: transition must return a Dict, got '" + result.typeName() + "'");
        // ... and it MUST carry both keys.
        if (!result.has("state"))
            throw KiritoError("fsm: transition result missing 'state' key");
        if (!result.has("ctx"))
            throw KiritoError("fsm: transition result missing 'ctx' key");

        // Adopt the next state (copy the String out of the by-value temporary immediately).
        std::string next = result.get("state").asStringRef("next state");
        Value nextCtx = result.get("ctx");
        if (!nextCtx.isDict())
            throw KiritoError("fsm: transition result 'ctx' must be a Dict");
        state_ = next;
        ctx_   = nextCtx.asDict("ctx");
    }

    const std::string& state() const { return state_; }
    Dict&              ctx()         { return ctx_; }

private:
    KiritoVM&   vm_;
    Handle      transition_;
    std::string state_;
    Dict        ctx_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // The turnstile transition. State is "locked" or "unlocked":
    //   locked   + coin  → unlocked   (bank the coin)
    //   unlocked + push  → locked     (count one pass)
    //   locked   + push  → locked     (GUARD: reject — stays locked, counts the rejection)
    //   unlocked + coin  → unlocked   (extra coin banked, no state change)
    // ctx keeps running counters: coins, passes, rejected.
    Handle turnstile = compile(R"KI(
Function(state, event, ctx) -> Dict:
    var kind = event["kind"]
    var next = state
    var coins = ctx["coins"]
    var passes = ctx["passes"]
    var rejected = ctx["rejected"]
    if kind == "coin":
        coins = coins + 1
        next = "unlocked"
    elif kind == "push":
        if state == "unlocked":
            passes = passes + 1
            next = "locked"
        else:
            rejected = rejected + 1
            next = "locked"
    var newctx = {"coins": coins, "passes": passes, "rejected": rejected}
    return {"state": next, "ctx": newctx}
)KI");

    StateMachine m(vm, turnstile, "locked");
    m.ctx().set("coins",    Value(vm, int64_t{0}));
    m.ctx().set("passes",   Value(vm, int64_t{0}));
    m.ctx().set("rejected", Value(vm, int64_t{0}));

    // A scripted sequence of events. Each event is a small Dict {"kind": ...}.
    auto ev = [&](const char* kind) {
        Dict d(vm);
        d.set("kind", Value(vm, kind));
        return Value(vm, d.handle());
    };

    // Scenario:
    //   push while locked  → rejected (stays locked)
    //   coin               → unlocked
    //   push               → one pass, back to locked
    //   coin               → unlocked
    //   coin               → still unlocked (extra coin banked)
    //   push               → second pass, back to locked
    //   push while locked  → rejected again (stays locked)
    const std::vector<std::string> script = {
        "push", "coin", "push", "coin", "coin", "push", "push"
    };
    for (const std::string& k : script)
        m.step(ev(k.c_str()));

    // ---- final state + context ----
    CHECK(m.state() == "locked");
    CHECK(m.ctx()["coins"].asInt("coins")       == 3);   // three coins fed
    CHECK(m.ctx()["passes"].asInt("passes")     == 2);   // two successful pushes
    CHECK(m.ctx()["rejected"].asInt("rejected") == 2);   // two pushes rejected while locked

    // ---- the guard really blocks a push from locked (isolated single-step check) ----
    {
        StateMachine g(vm, turnstile, "locked");
        g.ctx().set("coins",    Value(vm, int64_t{0}));
        g.ctx().set("passes",   Value(vm, int64_t{0}));
        g.ctx().set("rejected", Value(vm, int64_t{0}));
        g.step(ev("push"));
        CHECK(g.state() == "locked");                         // still locked
        CHECK(g.ctx()["passes"].asInt("passes") == 0);    // no pass granted
        CHECK(g.ctx()["rejected"].asInt("rejected") == 1);
    }

    // ---- a coin unlocks, then a push relocks (the happy path, one step at a time) ----
    {
        StateMachine h(vm, turnstile, "locked");
        h.ctx().set("coins",    Value(vm, int64_t{0}));
        h.ctx().set("passes",   Value(vm, int64_t{0}));
        h.ctx().set("rejected", Value(vm, int64_t{0}));
        h.step(ev("coin"));
        CHECK(h.state() == "unlocked");
        h.step(ev("push"));
        CHECK(h.state() == "locked");
        CHECK(h.ctx()["passes"].asInt("passes") == 1);
    }

    // ---- adversarial: a transition that omits the "state" key must throw ----
    {
        Handle bad = compile(R"KI(
Function(state, event, ctx) -> Dict:
    return {"ctx": ctx}
)KI");
        StateMachine b(vm, bad, "locked");
        b.ctx().set("coins", Value(vm, int64_t{0}));
        CHECK_THROWS(b.step(ev("coin")));
    }

    // ---- adversarial: a transition returning a non-Dict must throw ----
    {
        Handle bad = compile(R"KI(
Function(state, event, ctx):
    return "not-a-dict"
)KI");
        StateMachine b(vm, bad, "locked");
        b.ctx().set("coins", Value(vm, int64_t{0}));
        CHECK_THROWS(b.step(ev("coin")));
    }

    return RUN_TESTS();
}
