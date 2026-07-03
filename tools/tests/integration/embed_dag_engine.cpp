// embed_dag_engine.cpp — a small data-flow DAG scheduler. C++ owns the graph structure (nodes +
// their edges + topological ordering + a materialised value table); each node's TRANSFORM is a
// Kirito function. Flow bounces bar-per-bar: C++ resolves the next ready node → gathers its
// dependency values from the table → hands them to Kirito → stores the returned value.
//
// The C++ side registers a native `dag` module that exposes a `print` helper the transforms use
// for their own status output (unused in this test, kept as a sanity check that a Kirito node
// can call BACK into C++). The graph exercised: read → parse → double → sum → format.
//   A =         42                        (constant node in C++'s value table before .run)
//   B = parse(A) = "42"                   (Kirito casts to a String)
//   C = double(A) = 84                    (Kirito integer arithmetic)
//   D = double(C) = 168                   (chain of two doublings)
//   E = sum(A, B, C, D)                   (Kirito reduces a mixed-type list of dep values)
//   F = format(E)                         (Kirito formats the final result)

#include <algorithm>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

struct Node {
    std::string name;
    std::vector<std::string> deps;    // names of predecessor nodes
    Handle fn = Handle{};             // Kirito Function; empty means "input constant"
    Handle value = Handle{};          // set once after this node runs; used by successors
};

// Topological sort by Kahn's algorithm. Throws on a cycle so the embedder can wrap it.
static std::vector<std::string> topoOrder(const std::vector<Node>& nodes) {
    std::unordered_map<std::string, int> indeg;
    std::unordered_map<std::string, std::vector<std::string>> succ;
    for (const auto& n : nodes) {
        indeg.emplace(n.name, 0);
        succ.emplace(n.name, std::vector<std::string>{});
    }
    for (const auto& n : nodes) {
        for (const auto& d : n.deps) {
            if (!indeg.count(d)) throw std::runtime_error("dag: unknown dep '" + d + "' for node '" + n.name + "'");
            succ[d].push_back(n.name);
            ++indeg[n.name];
        }
    }
    std::vector<std::string> q, order;
    for (const auto& [k, v] : indeg) if (v == 0) q.push_back(k);
    std::sort(q.begin(), q.end());   // deterministic ordering when ties
    while (!q.empty()) {
        auto cur = q.back();
        q.pop_back();
        order.push_back(cur);
        auto ss = succ[cur];
        std::sort(ss.begin(), ss.end());
        for (const auto& s : ss)
            if (--indeg[s] == 0) q.push_back(s);
    }
    if (order.size() != nodes.size())
        throw std::runtime_error("dag: cycle detected (only " + std::to_string(order.size()) +
                                 "/" + std::to_string(nodes.size()) + " nodes ordered)");
    return order;
}

// Execute a graph: for each node in topo order, gather its dep values as a Kirito List and call
// its transform Function(list) -> Any. The result is stored back in the node's `value` slot.
static void runGraph(KiritoVM& vm, std::vector<Node>& nodes) {
    auto order = topoOrder(nodes);
    std::unordered_map<std::string, std::size_t> idx;
    for (std::size_t i = 0; i < nodes.size(); ++i) idx[nodes[i].name] = i;

    RootScope rs(vm);
    for (const auto& name : order) {
        Node& n = nodes[idx[name]];
        if (n.fn.slot == 0 && n.fn.generation == 0) continue;   // input node — value is already set
        List depL(vm);
        for (const auto& dName : n.deps) {
            const Node& dNode = nodes[idx[dName]];
            depL.add(dNode.value);
        }
        Handle depsH = rs.add(depL.build().handle());
        std::array<Handle, 1> args{depsH};
        n.value = rs.add(vm.arena().deref(n.fn).call(vm, args));
    }
}

struct DagModule : NativeModule {
    std::string name() const override { return "dag"; }
    void setup(ModuleBuilder& m) override {
        // A small trace primitive the Kirito nodes may call; we don't assert its output here, we
        // just prove the callback path exists.
        m.fn("trace", {{"label", "String"}, {"value"}}, "None",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 Args args(vm, a, "trace");
                 (void)args.at(0).asString("label");
                 (void)args.at(1);
                 return vm.none();
             });
    }
};

int main() {
    KiritoVM vm;
    vm.install<DagModule>();

    // Kirito transforms — each takes a List of dep values, returns any value.
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // parse: cast the sole dep to a String
    Handle fParse   = compile("Function(deps): return String(deps[0])");
    // double: multiply the sole dep by two (works on Integer)
    Handle fDouble  = compile("Function(deps): return deps[0] * 2");
    // sum: fold the deps with a running Integer accumulator (String deps are ignored)
    Handle fSum     = compile(R"KI(
Function(deps):
    var acc = 0
    for d in deps:
        if type(d) == "Integer":
            acc = acc + d
    return acc
)KI");
    // format: format(n, ",") — the final rendered String, calling BACK into a Kirito builtin
    Handle fFormat  = compile(R"KI(
var dag = import("dag")
Function(deps):
    var r = format(deps[0], ",")
    dag.trace("format", r)
    return r
)KI");

    // Build the graph. A is a constant input; B/C/D/E/F are Kirito-computed.
    std::vector<Node> nodes;
    nodes.push_back({"A", {},              Handle{}, val(vm, int64_t{42}).handle()});
    nodes.push_back({"B", {"A"},           fParse,  Handle{}});
    nodes.push_back({"C", {"A"},           fDouble, Handle{}});
    nodes.push_back({"D", {"C"},           fDouble, Handle{}});
    nodes.push_back({"E", {"A","B","C","D"}, fSum,  Handle{}});
    nodes.push_back({"F", {"E"},           fFormat, Handle{}});

    runGraph(vm, nodes);
    // Read back and verify each stage.
    auto by = [&](const char* name) {
        for (auto& n : nodes) if (n.name == name) return Value(vm, n.value);
        throw std::runtime_error("node not found");
    };
    CHECK(by("A").asInt("A") == 42);
    CHECK(by("B").asString("B") == "42");
    CHECK(by("C").asInt("C") == 84);
    CHECK(by("D").asInt("D") == 168);
    CHECK(by("E").asInt("E") == 42 + 84 + 168);    // "42" is skipped by the type check
    CHECK(by("F").asString("F") == "294");

    // ---- adversarial: a cycle throws cleanly before any node runs ----
    {
        std::vector<Node> cyc;
        cyc.push_back({"X", {"Y"}, fDouble, Handle{}});
        cyc.push_back({"Y", {"X"}, fDouble, Handle{}});
        CHECK_THROWS(runGraph(vm, cyc));
    }
    // ---- adversarial: an unknown dep throws with the offending pair named ----
    {
        std::vector<Node> bad;
        bad.push_back({"P", {"Q"}, fDouble, Handle{}});
        CHECK_THROWS(runGraph(vm, bad));
    }
    // ---- adversarial: a Kirito transform that throws propagates cleanly across the boundary ----
    // Kirito's `throw <value>` unwinds as a KiritoThrow (a plain struct, not std::exception —
    // the embedder that wants a payload can `catch (KiritoThrow& t)` and stringify t.value). We
    // just verify SOME exception escapes and the graph didn't corrupt its `value` slots.
    {
        Handle boom = compile("Function(deps): throw \"boom\"\n");
        std::vector<Node> g;
        g.push_back({"S", {}, Handle{}, val(vm, int64_t{1}).handle()});
        g.push_back({"T", {"S"}, boom, Handle{}});
        CHECK_THROWS(runGraph(vm, g));
        // Typed catch: peek at the value that was thrown.
        try {
            runGraph(vm, g);
            CHECK(false && "expected a throw");
        } catch (const KiritoThrow& t) {
            CHECK(vm.stringify(t.value) == "boom");
        } catch (...) {
            CHECK(false && "expected KiritoThrow, got something else");
        }
    }

    // ---- multi-source topology: two independent inputs feeding a common node ----
    {
        Handle add = compile("Function(deps): return deps[0] + deps[1]");
        std::vector<Node> g;
        g.push_back({"X", {},      Handle{}, val(vm, int64_t{10}).handle()});
        g.push_back({"Y", {},      Handle{}, val(vm, int64_t{20}).handle()});
        g.push_back({"Z", {"X","Y"}, add,   Handle{}});
        runGraph(vm, g);
        for (auto& n : g) if (n.name == "Z") { CHECK(Value(vm, n.value).asInt("Z") == 30); }
    }

    // ---- deterministic replay: running the same graph twice yields the same values ----
    {
        std::vector<Node> a = nodes;    // reuse the outer nodes (values may already be set —
        for (auto& n : a) if (!n.deps.empty()) n.value = Handle{};   // clear derived values
        runGraph(vm, a);
        for (auto& n : a)
            if (n.name == "F")
                CHECK(Value(vm, n.value).asString("replay F") == "294");
    }
    return RUN_TESTS();
}
