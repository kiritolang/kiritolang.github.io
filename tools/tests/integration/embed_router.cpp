// embed_router.cpp — a pub/sub event router whose ROUTING is Kirito. C++ owns the topics (each a
// vector<Msg> mailbox) and the event queue; each router rule is a Kirito
// Function(event: Dict) -> List of String — the topics the event should be delivered to. Rules
// compose (a rule may return "reject") and rules may TRANSFORM an event by returning a Dict
// action `{"topic": ..., "event": <modified>}` instead of a bare topic name.
//
// Flow per event: C++ (dequeue) → for each rule → Kirito (routing decision) → C++ (deliver
// to mailboxes).

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

struct Msg {
    std::string type;
    std::string payload;
    int64_t     priority;   // any Integer field
};

// Convert an Msg to/from a Kirito Dict — the router hands the event to Kirito, receives back a
// (possibly transformed) event Dict.
static Handle eventToDict(KiritoVM& vm, const Msg& e) {
    Dict d(vm);
    d.set("type",     Value(vm, e.type));
    d.set("payload",  Value(vm, e.payload));
    d.set("priority", Value(vm, e.priority));
    return d.build().handle();
}
static Msg dictToEvent(Value d) {
    return { d.get("type").asStringRef("type"),
             d.get("payload").asStringRef("payload"),
             d.get("priority").asInt("priority") };
}

class Router {
public:
    explicit Router(KiritoVM& vm) : vm_(vm) {}
    void addRule(Handle fn) { rules_.push_back(fn); }
    void dispatch(const Msg& e) {
        RootScope rs(vm_);
        Handle evH = rs.add(eventToDict(vm_, e));
        Msg current = e;
        for (Handle rH : rules_) {
            std::array<Handle, 1> args{evH};
            Handle rulesH = rs.add(vm_.arena().deref(rH).call(vm_, args));
            Value result(vm_, rulesH);
            // A rule MUST return a List (or the router iterates the wrong shape). Fail loudly.
            if (!result.isList())
                throw KiritoError("router: rule must return a List, got '" + result.typeName() + "'");
            for (Value item : result.items()) {
                if (item.isString()) {
                    const std::string& topic = item.asStringRef("topic");
                    if (topic == "reject") return;
                    topics_[topic].push_back(current);
                } else if (item.isDict()) {
                    // {"topic": String, "event": Dict} — transform the event AND route it.
                    // Copy the topic String out of the temporary before it dies (Value::get
                    // returns by value, so the bound `asString` reference is dangling otherwise).
                    std::string topic = item.get("topic").asStringRef("routed topic");
                    Value evtView = item.get("event");
                    Msg transformed = dictToEvent(evtView);
                    topics_[topic].push_back(transformed);
                    current = transformed;
                    // update evH so downstream rules see the transform
                    evH = rs.add(eventToDict(vm_, current));
                } else {
                    throw KiritoError("router: rule must return a List of Strings or {topic,event} Dicts");
                }
            }
        }
    }
    const std::vector<Msg>& box(const std::string& topic) const {
        static const std::vector<Msg> empty;
        auto it = topics_.find(topic);
        return it == topics_.end() ? empty : it->second;
    }

private:
    KiritoVM& vm_;
    std::vector<Handle> rules_;
    std::unordered_map<std::string, std::vector<Msg>> topics_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    Router r(vm);
    // Rule 1: everything with priority >= 10 routes to "high"; otherwise no opinion (empty list).
    r.addRule(compile(R"KI(
Function(ev) -> List:
    if ev["priority"] >= 10:
        return ["high"]
    return []
)KI"));
    // Rule 2: route by TYPE — "log"/"trace" → "logs" mailbox; "error" → "logs" AND "alerts".
    r.addRule(compile(R"KI(
Function(ev) -> List:
    if ev["type"] == "log" or ev["type"] == "trace":
        return ["logs"]
    if ev["type"] == "error":
        return ["logs", "alerts"]
    return []
)KI"));
    // Rule 3: TRANSFORM — every event also lands on "audit" with the payload prefixed. Uses the
    // {"topic": ..., "event": ...} shape so downstream inspectors would see the modified event.
    r.addRule(compile(R"KI(
Function(ev) -> List:
    var mod = {"type": ev["type"], "payload": "AUDIT:" + ev["payload"], "priority": ev["priority"]}
    return [{"topic": "audit", "event": mod}]
)KI"));

    r.dispatch({"log",   "user-login",   1});
    r.dispatch({"error", "disk-full",    99});
    r.dispatch({"trace", "cache-miss",   3});
    r.dispatch({"metric","cpu=48",       5});

    // ---- verifications ----
    // "high" gets only the disk-full error (priority 99)
    CHECK(r.box("high").size() == 1);
    CHECK(r.box("high").at(0).type == "error");

    // "logs" gets log + trace + error
    CHECK(r.box("logs").size() == 3);
    // "alerts" gets only the error
    CHECK(r.box("alerts").size() == 1);
    CHECK(r.box("alerts").at(0).payload == "disk-full");

    // "audit" gets EVERY event with the payload prefix
    CHECK(r.box("audit").size() == 4);
    for (const auto& e : r.box("audit"))
        CHECK(e.payload.rfind("AUDIT:", 0) == 0);
    // A topic not mentioned by any rule stays empty
    CHECK(r.box("nowhere").empty());

    // ---- rejection short-circuits: a rule returning "reject" drops the event ----
    {
        Router r2(vm);
        r2.addRule(compile(R"KI(
Function(ev) -> List:
    if ev["type"] == "banned":
        return ["reject"]
    return ["all"]
)KI"));
        r2.dispatch({"ok",     "keep-me",  0});
        r2.dispatch({"banned", "drop-me",  0});
        CHECK(r2.box("all").size() == 1);
        CHECK(r2.box("all").at(0).payload == "keep-me");
    }

    // ---- adversarial: rule that returns a non-list throws cleanly ----
    {
        Router r3(vm);
        r3.addRule(compile("Function(ev): return \"not-a-list\"\n"));
        CHECK_THROWS(r3.dispatch({"x", "y", 0}));
    }

    // ---- adversarial: transform emits a malformed Dict (missing "event") ----
    {
        Router r4(vm);
        r4.addRule(compile("Function(ev): return [{\"topic\": \"a\"}]\n"));
        CHECK_THROWS(r4.dispatch({"x", "y", 0}));
    }

    // ---- deterministic ordering: rules run in registration order, delivery goes into
    //      per-topic vectors in dispatch order ----
    {
        Router r5(vm);
        r5.addRule(compile("Function(ev): return [\"order\"]\n"));
        for (int i = 0; i < 5; ++i) r5.dispatch({"n", std::to_string(i), i});
        CHECK(r5.box("order").size() == 5);
        for (std::size_t i = 0; i < 5; ++i)
            CHECK(r5.box("order").at(i).payload == std::to_string(i));
    }

    return RUN_TESTS();
}
