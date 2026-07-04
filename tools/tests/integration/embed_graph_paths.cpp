// embed_graph_paths.cpp — a shortest-path engine with a pluggable cost model. C++ owns the graph
// (a directed weighted adjacency map) and runs Dijkstra; Kirito owns the EDGE COST — a
// Function(edge: Dict) -> Float given {"from","to","base","traffic"}. Swapping the cost model
// (raw distance vs distance*(1+traffic)) changes which route is shortest.
//
// Flow per relaxation: C++ (pop node) → build edge Dict → Kirito (cost model) → C++ (validate the
// weight is a non-negative number, relax). Dijkstra requires non-negative weights, so a model that
// returns a negative or non-numeric cost is rejected loudly.

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

struct Edge {
    std::string to;
    double      base;      // raw distance
    double      traffic;   // congestion factor (>= 0)
};

struct PathResult {
    std::vector<std::string> path;   // src..dst inclusive; empty if unreachable
    double                   cost;   // total weight along `path`
    bool                     found;
};

// A directed weighted graph whose edge weights come from a Kirito cost model. C++ keeps the
// structure; each `shortest()` call weights the same edges through whatever Kirito Function is
// passed in.
class Graph {
public:
    explicit Graph(KiritoVM& vm) : vm_(vm) {}

    void addEdge(const std::string& from, const std::string& to, double base, double traffic) {
        adj_[from].push_back(Edge{to, base, traffic});
        adj_.try_emplace(to);   // ensure `to` is a known node even with no outgoing edges
    }

    // Ask the Kirito cost model for this edge's weight, validating it in C++.
    double weight(const std::string& from, const Edge& e, Handle cost) {
        Dict d(vm_);
        d.set("from",    Value(vm_, from));
        d.set("to",      Value(vm_, e.to));
        d.set("base",    Value(vm_, e.base));
        d.set("traffic", Value(vm_, e.traffic));

        std::array<Handle, 1> args{d.handle()};
        Handle rH = vm_.arena().deref(cost).call(vm_, args);
        Value r(vm_, rH);

        // A cost model MUST yield a number — a String / None / List weight is meaningless.
        if (!r.isNumber())
            throw KiritoError("cost model must return a number, got '" + r.typeName() + "'");
        double w = r.asFloat("edge cost");
        // Dijkstra's correctness depends on non-negative weights; reject anything else.
        if (w < 0.0)
            throw KiritoError("cost model returned a negative weight; Dijkstra requires non-negative edge costs");
        return w;
    }

    // Dijkstra from `src` to `dst` using `cost` to weight every relaxed edge.
    PathResult shortest(const std::string& src, const std::string& dst, Handle cost) {
        const double INF = std::numeric_limits<double>::infinity();
        std::unordered_map<std::string, double>      dist;
        std::unordered_map<std::string, std::string> prev;
        for (const auto& [node, _] : adj_) dist[node] = INF;
        if (!dist.count(src)) dist[src] = INF;

        using Item = std::pair<double, std::string>;   // (distance, node); tie-broken by node name
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
        dist[src] = 0.0;
        pq.push({0.0, src});

        while (!pq.empty()) {
            auto [d, u] = pq.top();
            pq.pop();
            if (d > dist[u]) continue;      // stale entry
            if (u == dst) break;            // dst finalized
            auto it = adj_.find(u);
            if (it == adj_.end()) continue;
            for (const Edge& e : it->second) {
                double nd = d + weight(u, e, cost);
                if (nd < dist[e.to]) {
                    dist[e.to] = nd;
                    prev[e.to] = u;
                    pq.push({nd, e.to});
                }
            }
        }

        if (dist[dst] == INF) return {{}, INF, false};
        std::vector<std::string> path;
        for (std::string at = dst; ; at = prev[at]) {
            path.push_back(at);
            if (at == src) break;
        }
        std::reverse(path.begin(), path.end());
        return {path, dist[dst], true};
    }

private:
    KiritoVM&                                             vm_;
    std::unordered_map<std::string, std::vector<Edge>>    adj_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // A small hand-built graph. Two routes A -> D:
    //   via B: A->B (base 1, traffic 5), B->D (base 1, traffic 0)
    //   via C: A->C (base 2, traffic 0), C->D (base 2, traffic 0)
    Graph g(vm);
    g.addEdge("A", "B", 1.0, 5.0);
    g.addEdge("B", "D", 1.0, 0.0);
    g.addEdge("A", "C", 2.0, 0.0);
    g.addEdge("C", "D", 2.0, 0.0);

    // Cost model 1 — RAW distance: ignore traffic entirely.
    Handle rawModel = compile(R"KI(
Function(edge : Dict) -> Float:
    return edge["base"]
)KI");
    // Cost model 2 — TRAFFIC-AWARE: penalize congested edges.
    Handle trafficModel = compile(R"KI(
Function(edge : Dict) -> Float:
    return edge["base"] * (1.0 + edge["traffic"])
)KI");

    // ---- raw distance prefers the B route ----
    // via B: 1 + 1 = 2 ; via C: 2 + 2 = 4  ->  [A,B,D] wins at cost 2.
    PathResult raw = g.shortest("A", "D", rawModel);
    CHECK(raw.found);
    CHECK(raw.path == (std::vector<std::string>{"A", "B", "D"}));
    CHECK(Float(vm, raw.cost).compare(2.0));

    // ---- traffic-aware flips the answer to the C route ----
    // via B: 1*(1+5) + 1*(1+0) = 6 + 1 = 7 ; via C: 2*1 + 2*1 = 4  ->  [A,C,D] wins at cost 4.
    PathResult heavy = g.shortest("A", "D", trafficModel);
    CHECK(heavy.found);
    CHECK(heavy.path == (std::vector<std::string>{"A", "C", "D"}));
    CHECK(Float(vm, heavy.cost).compare(4.0));

    // ---- the two models genuinely disagree: different route AND different total ----
    CHECK(raw.path != heavy.path);
    CHECK(!Float(vm, raw.cost).compare(heavy.cost));

    // ---- an unreachable destination reports no path (no node has an edge into "Z") ----
    g.addEdge("Z", "A", 1.0, 0.0);   // Z is a source-only sink target; nothing points at Z
    PathResult unreachable = g.shortest("A", "Z", rawModel);
    CHECK(!unreachable.found);
    CHECK(unreachable.path.empty());

    // ---- a trivial same-node query costs nothing ----
    PathResult self = g.shortest("A", "A", rawModel);
    CHECK(self.found);
    CHECK(self.path == (std::vector<std::string>{"A"}));
    CHECK(Float(vm, self.cost).compare(0.0));

    // ---- adversarial: a cost model returning a NEGATIVE weight is rejected by C++ ----
    // (Dijkstra requires non-negative edge weights; C++ throws when it sees w < 0.)
    {
        Handle negModel = compile(R"KI(
Function(edge : Dict) -> Float:
    return -edge["base"]
)KI");
        CHECK_THROWS(g.shortest("A", "D", negModel));
    }

    // ---- adversarial: a cost model returning a NON-NUMBER throws ----
    // (unannotated, so Kirito hands back a String and the C++ isNumber() guard fires.)
    {
        Handle badModel = compile("Function(edge): return \"toll-booth\"\n");
        CHECK_THROWS(g.shortest("A", "D", badModel));
    }

    // ---- adversarial: a cost model that indexes a MISSING key throws (bad key inside Kirito) ----
    {
        Handle keyModel = compile(R"KI(
Function(edge : Dict) -> Float:
    return edge["distance"]
)KI");
        CHECK_THROWS(g.shortest("A", "D", keyModel));
    }

    return RUN_TESTS();
}
