// embed_strat_sim.cpp — a small OHLC trading-strategy backtester whose STRATEGY is a Kirito
// closure. The C++ engine owns the price series, the portfolio, and PnL bookkeeping; every bar it
// hands the Kirito `on_bar(bar) -> Dict` a compact bar dict, then acts on whatever action the
// strategy returns ({"action": "buy" | "sell" | "hold", "qty"?: Integer}).
//
// Flow (per bar): C++ → Kirito → C++ → Kirito → …
//   C++ deterministically generates a synthetic OHLC bar (fixed formula, no PRNG so the test is
//   stable). C++ calls the Kirito strategy with a Dict describing the bar. Kirito returns an
//   action Dict. C++ mutates the portfolio + logs the fill; on the next bar it repeats.
//
// The strategies exercised:
//   - BuyAndHold  — buys on the first bar, holds thereafter.
//   - SMACrossover — buys when short SMA crosses above long SMA, sells when it crosses back.
//   - AlwaysSell  — always signals sell (nothing to sell → no trade); pins the empty-portfolio
//                   path.
// C++ verifies exact PnL for BuyAndHold, sane invariants for the other two, and that a bad
// strategy return (wrong-typed action or qty) throws cleanly across the C++/Kirito boundary.

#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

struct Bar {
    int64_t t;
    double open, high, low, close;
    int64_t volume;
};

// A deterministic sinusoidal price series so SMA crossovers fire a known number of times.
static std::vector<Bar> makeSeries(int64_t n) {
    std::vector<Bar> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int64_t t = 0; t < n; ++t) {
        double c = 100.0 + 15.0 * std::sin(static_cast<double>(t) * 0.35) + 0.05 * static_cast<double>(t);
        double o = 100.0 + 15.0 * std::sin(static_cast<double>(t - 1) * 0.35) + 0.05 * static_cast<double>(t - 1);
        double h = std::max(o, c) + 0.5;
        double l = std::min(o, c) - 0.5;
        out.push_back({t, o, h, l, c, 1000});
    }
    return out;
}

// Pack a bar into a Kirito Dict (freshly allocated per call — cheap enough for a backtest).
static Handle barToDict(KiritoVM& vm, const Bar& b) {
    Dict d(vm);
    d.set("t",      Value(vm, b.t));
    d.set("open",   Value(vm, b.open));
    d.set("high",   Value(vm, b.high));
    d.set("low",    Value(vm, b.low));
    d.set("close",  Value(vm, b.close));
    d.set("volume", Value(vm, b.volume));
    return d.build().handle();
}

struct BacktestResult {
    double  equity;      // mark-to-market at the last close
    int64_t trades;
    double  cash;
    int64_t position;
    std::string lastSignal;
};

// Drive one Kirito strategy closure across a bar series. The closure captures its own state (e.g.
// the SMA window of past closes), so C++ only has to hand it one bar Dict per call.
static BacktestResult run(KiritoVM& vm, Handle strategyH, const std::vector<Bar>& bars,
                          double startingCash) {
    BacktestResult r{startingCash, 0, startingCash, 0, "hold"};
    RootScope rs(vm);
    Handle sH = rs.add(strategyH);
    for (const Bar& b : bars) {
        Handle barH = rs.add(barToDict(vm, b));
        std::array<Handle, 1> args{barH};
        Handle resH = rs.add(vm.arena().deref(sH).call(vm, args));
        Value res(vm, resH);
        // The strategy MUST return a Dict — the {"action": ..., "qty"?: ...} contract.
        std::string action = res.get("action").asStringRef("strategy action");
        int64_t qty = res.has("qty") ? res.get("qty").asInt("strategy qty") : int64_t{1};
        r.lastSignal = action;
        if (action == "buy" && r.cash >= b.close * static_cast<double>(qty)) {
            r.cash -= b.close * static_cast<double>(qty);
            r.position += qty;
            ++r.trades;
        } else if (action == "sell" && r.position >= qty) {
            r.cash += b.close * static_cast<double>(qty);
            r.position -= qty;
            ++r.trades;
        }
    }
    r.equity = r.cash + static_cast<double>(r.position) * bars.back().close;
    return r;
}

// A tiny native module the Kirito strategies can call back into — shows that flow bounces from
// C++ (bar arrives) → Kirito (strategy sees it) → C++ (strat.sma() called) → Kirito (uses result).
struct StratModule : NativeModule {
    std::string name() const override { return "strat"; }
    void setup(ModuleBuilder& m) override {
        m.fn("sma", {{"values", "List"}, {"window", "Integer"}}, "Float",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 Args args(vm, a, "sma");
                 int64_t w = args.at(1).asInt("window");
                 if (w <= 0) throw KiritoError("sma window must be > 0");
                 auto items = args.at(0).items();
                 int64_t n = static_cast<int64_t>(items.size());
                 int64_t take = std::min(n, w);
                 if (take == 0) return Value(vm, 0.0);
                 double sum = 0.0;
                 for (int64_t i = n - take; i < n; ++i) sum += items[static_cast<std::size_t>(i)].asFloat("value");
                 return Value(vm, sum / static_cast<double>(take));
             });
    }
};

int main() {
    KiritoVM vm;
    vm.install<StratModule>();

    // Every strategy is a FACTORY that returns a closure holding its own state. The last
    // expression of the source is the factory itself (not a call to it); we then call the
    // factory from C++ to get a fresh strategy.
    const char* buyAndHold = R"KI(
Function():
    var bought = False
    return Function(bar) -> Dict:
        if bought:
            return {"action": "hold"}
        bought = True
        return {"action": "buy", "qty": 1}
)KI";

    const char* smaCross = R"KI(
var strat = import("strat")
Function(shortW, longW):
    var closes = []
    var position = 0
    return Function(bar) -> Dict:
        closes.append(bar["close"])
        if len(closes) < longW:
            return {"action": "hold"}
        var s = strat.sma(closes, shortW)
        var l = strat.sma(closes, longW)
        if s > l and position == 0:
            position = 5
            return {"action": "buy", "qty": 5}
        if s < l and position > 0:
            var q = position
            position = 0
            return {"action": "sell", "qty": q}
        return {"action": "hold"}
)KI";

    const char* alwaysSell = R"KI(
Function():
    return Function(bar) -> Dict:
        return {"action": "sell", "qty": 100}
)KI";

    auto series = makeSeries(40);

    // Helper: run source → returns factory → call factory with args → returns the strategy closure.
    auto makeStrategy = [&](const char* src, std::vector<Handle> args) {
        Handle factory = vm.runSource(src);
        RootScope rs(vm);
        Handle f = rs.add(factory);
        return vm.arena().deref(f).call(vm, args);
    };

    // 1) buy-and-hold: exactly one buy on the first bar, then hold; cash and position update once.
    {
        Handle sH = makeStrategy(buyAndHold, {});
        BacktestResult r = run(vm, sH, series, 1000.0);
        CHECK(r.trades == 1);
        CHECK(r.position == 1);
        CHECK(std::abs(r.cash - (1000.0 - series[0].close)) < 1e-6);
        CHECK(std::abs(r.equity - (r.cash + series.back().close)) < 1e-6);
        CHECK(r.lastSignal == "hold");
    }

    // 2) SMA crossover: at least one buy + one sell, ending finite and non-negative cash.
    {
        Handle sH = makeStrategy(smaCross, {vm.makeInt(3), vm.makeInt(12)});
        BacktestResult r = run(vm, sH, series, 5000.0);
        CHECK(r.trades >= 2);
        CHECK(r.trades <= 10);
        CHECK(std::isfinite(r.equity));
        CHECK(r.cash >= 0.0);
    }

    // 3) always-sell against an empty portfolio: no trades, cash preserved, lastSignal reflects
    //    the strategy's actual choice even though the engine refused to execute.
    {
        Handle sH = makeStrategy(alwaysSell, {});
        BacktestResult r = run(vm, sH, series, 500.0);
        CHECK(r.trades == 0);
        CHECK(r.position == 0);
        CHECK(std::abs(r.cash - 500.0) < 1e-9);
        CHECK(r.lastSignal == "sell");
        CHECK(std::abs(r.equity - 500.0) < 1e-9);
    }

    // 4) Deterministic replay: two independent factory invocations on the same series must yield
    //    the same trade count + PnL (no leaked state between strategy instances).
    {
        Handle a = makeStrategy(smaCross, {vm.makeInt(3), vm.makeInt(12)});
        BacktestResult r1 = run(vm, a, series, 5000.0);
        Handle b = makeStrategy(smaCross, {vm.makeInt(3), vm.makeInt(12)});
        BacktestResult r2 = run(vm, b, series, 5000.0);
        CHECK(r1.trades == r2.trades);
        CHECK(std::abs(r1.equity - r2.equity) < 1e-9);
    }

    // 5) Hostile Kirito returns: the C++ engine must surface a clean throw (never crash), so the
    //    embedder can wrap the whole run() in a try/catch.
    {
        // (a) returns a String — get("action") never even runs; asDictRef throws first.
        Handle strS = makeStrategy(R"KI(Function(): return Function(bar): return "nope")KI", {});
        CHECK_THROWS(run(vm, strS, series, 100.0));
        // (b) returns a Dict with a wrong-typed qty.
        Handle strQty = makeStrategy(
            R"KI(Function(): return Function(bar): return {"action": "buy", "qty": "lots"})KI", {});
        CHECK_THROWS(run(vm, strQty, series, 100.0));
        // (c) returns a Dict missing the "action" key.
        Handle strNoAct = makeStrategy(
            R"KI(Function(): return Function(bar): return {"qty": 1})KI", {});
        CHECK_THROWS(run(vm, strNoAct, series, 100.0));
    }

    return RUN_TESTS();
}
