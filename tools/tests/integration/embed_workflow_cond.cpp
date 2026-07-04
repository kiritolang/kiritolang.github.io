// embed_workflow_cond.cpp — a conditional CI-style pipeline runner. C++ owns the ordered steps and
// the shared context Dict; Kirito owns each step's GATE (should this step run?) and ACTION (produce
// the next context). C++ walks the steps, skips any whose gate is false, applies the surviving
// actions, and records which steps executed — a build → test → (deploy only if tests passed and the
// branch is "main") flow.
//
// Flow per step: C++ (hand ctx to gate) → Kirito (Bool) → if true: C++ (hand ctx to action) →
// Kirito (new ctx Dict) → C++ (adopt it, record the step).

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// A pipeline step: a name plus two Kirito function values — a gate and an action.
struct Step {
    std::string name;
    Handle      gate;    // Function(ctx: Dict) -> Bool
    Handle      action;  // Function(ctx: Dict) -> Dict
};

class Pipeline {
public:
    explicit Pipeline(KiritoVM& vm) : vm_(vm) {}

    void addStep(const std::string& name, Handle gate, Handle action) {
        steps_.push_back({name, gate, action});
    }

    // Run the whole flow over a starting context. Returns the final context Dict handle; the list of
    // executed step names is available via executed().
    Handle run(Handle startCtx) {
        RootScope rs(vm_);
        Handle ctxH = rs.add(startCtx);
        executed_.clear();
        for (const Step& s : steps_) {
            // --- gate: decide whether to run this step ---
            std::array<Handle, 1> gateArgs{ctxH};
            Handle gH = rs.add(vm_.arena().deref(s.gate).call(vm_, gateArgs));
            Value gate(vm_, gH);
            if (!gate.isBool())
                throw KiritoError("pipeline: gate for '" + s.name + "' must return a Bool, got '" +
                                  gate.typeName() + "'");
            if (!gate.asBool("gate"))
                continue;  // gate said no — skip, context unchanged

            // --- action: produce the next context ---
            std::array<Handle, 1> actArgs{ctxH};
            Handle aH = rs.add(vm_.arena().deref(s.action).call(vm_, actArgs));
            Value next(vm_, aH);
            if (!next.isDict())
                throw KiritoError("pipeline: action for '" + s.name + "' must return a Dict, got '" +
                                  next.typeName() + "'");
            ctxH = rs.add(aH);          // adopt the new context for the following steps
            executed_.push_back(s.name);
        }
        // Hand the final context back out, rooted by the caller's own scope afterwards.
        return ctxH;
    }

    const std::vector<std::string>& executed() const { return executed_; }

private:
    KiritoVM&                vm_;
    std::vector<Step>        steps_;
    std::vector<std::string> executed_;
};

// Build a fresh pipeline modelling build → test → deploy. The gates/actions are pure Kirito.
static void wireBuildTestDeploy(KiritoVM& vm, Pipeline& p) {
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // build: always runs; marks the context built and records an artifact count.
    Handle buildGate = compile("Function(ctx) -> Bool:\n    return True\n");
    Handle buildAction = compile(R"KI(
Function(ctx) -> Dict:
    ctx.set("built", True)
    ctx.set("artifacts", 3)
    return ctx
)KI");

    // test: runs only if the build succeeded; passes when there are artifacts to test.
    Handle testGate = compile(R"KI(
Function(ctx) -> Bool:
    return ctx.get("built", False)
)KI");
    Handle testAction = compile(R"KI(
Function(ctx) -> Dict:
    ctx.set("tested", True)
    ctx.set("tests_passed", ctx.get("artifacts", 0) > 0)
    return ctx
)KI");

    // deploy: runs ONLY if tests passed AND the branch is "main".
    Handle deployGate = compile(R"KI(
Function(ctx) -> Bool:
    return ctx.get("tests_passed", False) and ctx.get("branch", "") == "main"
)KI");
    Handle deployAction = compile(R"KI(
Function(ctx) -> Dict:
    ctx.set("deployed", True)
    ctx.set("url", "https://" + ctx.get("branch", "") + ".example.com")
    return ctx
)KI");

    p.addStep("build",  buildGate,  buildAction);
    p.addStep("test",   testGate,   testAction);
    p.addStep("deploy", deployGate, deployAction);
}

// A starting context for a given branch.
static Handle startContext(KiritoVM& vm, const std::string& branch) {
    Dict d(vm);
    d.set("branch", Value(vm, branch));
    d.set("built", Value(vm, false));
    d.set("tested", Value(vm, false));
    d.set("deployed", Value(vm, false));
    return d.handle();
}

int main() {
    KiritoVM vm;

    // ---- scenario 1: branch == "main" — build, test, AND deploy all run ----
    {
        Pipeline p(vm);
        wireBuildTestDeploy(vm, p);

        RootScope rs(vm);
        Handle finalH = rs.add(p.run(startContext(vm, "main")));
        Value ctx(vm, finalH);

        // every step executed, in order
        const auto& ran = p.executed();
        CHECK(ran.size() == 3);
        CHECK(ran.at(0) == "build");
        CHECK(ran.at(1) == "test");
        CHECK(ran.at(2) == "deploy");

        // final context reflects a full, deployed run
        CHECK(ctx.get("built").asBool("built") == true);
        CHECK(ctx.get("tested").asBool("tested") == true);
        CHECK(ctx.get("tests_passed").asBool("tests_passed") == true);
        CHECK(ctx.get("deployed").asBool("deployed") == true);
        CHECK(ctx.get("artifacts").asInt("artifacts") == 3);
        CHECK(ctx.get("url").asStringRef("url") == "https://main.example.com");
    }

    // ---- scenario 2: branch == "feature" — build + test run, deploy is gated out ----
    {
        Pipeline p(vm);
        wireBuildTestDeploy(vm, p);

        RootScope rs(vm);
        Handle finalH = rs.add(p.run(startContext(vm, "feature")));
        Value ctx(vm, finalH);

        const auto& ran = p.executed();
        CHECK(ran.size() == 2);
        CHECK(ran.at(0) == "build");
        CHECK(ran.at(1) == "test");

        CHECK(ctx.get("built").asBool("built") == true);
        CHECK(ctx.get("tested").asBool("tested") == true);
        CHECK(ctx.get("tests_passed").asBool("tests_passed") == true);
        // deploy never ran: still the starting False, and no url key was ever set
        CHECK(ctx.get("deployed").asBool("deployed") == false);
        CHECK(ctx.has("url") == false);
    }

    // ---- scenario 3: a failing gate short-circuits the rest ----
    // If the build gate is False, test's gate (which reads "built") stays False too, so NOTHING runs.
    {
        Pipeline p(vm);
        auto compile = [&](const char* src) { return vm.runSource(src); };
        Handle noGate = compile("Function(ctx) -> Bool:\n    return False\n");
        Handle idAction = compile("Function(ctx) -> Dict:\n    return ctx\n");
        p.addStep("build", noGate, idAction);
        Handle gate = compile("Function(ctx) -> Bool:\n    return ctx.get(\"built\", False)\n");
        p.addStep("test", gate, idAction);

        RootScope rs(vm);
        Handle finalH = rs.add(p.run(startContext(vm, "main")));
        Value ctx(vm, finalH);
        CHECK(p.executed().empty());
        CHECK(ctx.get("built").asBool("built") == false);
    }

    // ---- adversarial: an action returning a non-Dict must throw ----
    {
        Pipeline p(vm);
        auto compile = [&](const char* src) { return vm.runSource(src); };
        Handle yes = compile("Function(ctx) -> Bool:\n    return True\n");
        // action returns an Integer instead of a Dict — the runner must reject it loudly.
        Handle badAction = compile("Function(ctx):\n    return 42\n");
        p.addStep("bad", yes, badAction);
        CHECK_THROWS(p.run(startContext(vm, "main")));
    }

    // ---- adversarial: a gate returning a non-Bool must throw ----
    {
        Pipeline p(vm);
        auto compile = [&](const char* src) { return vm.runSource(src); };
        Handle badGate = compile("Function(ctx):\n    return \"maybe\"\n");
        Handle idAction = compile("Function(ctx) -> Dict:\n    return ctx\n");
        p.addStep("weird", badGate, idAction);
        CHECK_THROWS(p.run(startContext(vm, "main")));
    }

    return RUN_TESTS();
}
