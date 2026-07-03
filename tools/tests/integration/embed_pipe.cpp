// embed_pipe.cpp — a streaming data pipeline. C++ owns the source stream (a bounded vector of
// items) and the sink (a collector); the intermediate stages are Kirito Function(item) -> Any
// transforms. A stage may FILTER by returning a special sentinel Handle (`vm.none()` — mapped to
// "drop this item"), TRANSFORM by returning any other value, or FAN-OUT by returning a List
// (each element continues down the pipeline). The engine chains them so a fan-out stage before a
// filter really does emit multiple items.
//
// Flow per item: C++ (source.next) → for each stage → Kirito (transform) → C++ (routing) →
// Kirito (next stage) → C++ (sink.append) — repeated `n_items × n_stages` times.

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

class Pipeline {
public:
    explicit Pipeline(KiritoVM& vm) : vm_(vm) {}
    void addStage(Handle fn) { stages_.push_back(fn); }
    // Push every element of `src` through every stage, sinking whatever survives.
    void run(const std::vector<Handle>& src) {
        for (Handle x : src) drive({x});
    }
    const std::vector<Handle>& sink() const { return sink_; }

private:
    void drive(std::vector<Handle> items) {
        for (Handle stage : stages_) {
            std::vector<Handle> next;
            RootScope rs(vm_);
            Handle sH = rs.add(stage);
            for (Handle it : items) {
                std::array<Handle, 1> args{rs.add(it)};
                Handle outH = rs.add(vm_.arena().deref(sH).call(vm_, args));
                Value out(vm_, outH);
                if (out.isNone()) continue;                       // filter drops
                if (out.isList()) {
                    for (Value v : out.items()) next.push_back(v.handle());   // fan-out
                } else {
                    next.push_back(outH);                          // transform
                }
            }
            items = std::move(next);
            if (items.empty()) return;
        }
        for (Handle x : items) sink_.push_back(x);
    }
    KiritoVM& vm_;
    std::vector<Handle> stages_;
    std::vector<Handle> sink_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // Stage 1: keep even Integers, drop odd (filter — returns None to signal "drop").
    Handle sKeepEven = compile(R"KI(
Function(x):
    if type(x) != "Integer":
        return None
    if x % 2 == 1:
        return None
    return x
)KI");
    // Stage 2: double each survivor (transform).
    Handle sDouble = compile("Function(x): return x * 2\n");
    // Stage 3: fan-out — emit both x AND x+1.
    Handle sFanout = compile("Function(x): return [x, x + 1]\n");
    // Stage 4: format as a String (final projection).
    Handle sFmt = compile("Function(x): return \"v=\" + String(x)\n");

    Pipeline p(vm);
    p.addStage(sKeepEven);
    p.addStage(sDouble);
    p.addStage(sFanout);
    p.addStage(sFmt);

    std::vector<Handle> input;
    for (int64_t i = 0; i < 8; ++i) input.push_back(val(vm, i).handle());
    p.run(input);

    // 0,1,2,3,4,5,6,7 → keep even → 0,2,4,6 → double → 0,4,8,12 → fanout → 0,1,4,5,8,9,12,13
    // → "v=0","v=1","v=4","v=5","v=8","v=9","v=12","v=13"
    const auto& out = p.sink();
    CHECK(out.size() == 8);
    std::vector<std::string> got;
    for (Handle h : out) got.push_back(Value(vm, h).asString(""));
    std::vector<std::string> want{"v=0","v=1","v=4","v=5","v=8","v=9","v=12","v=13"};
    CHECK(got == want);

    // ---- empty input yields empty sink ----
    {
        Pipeline p2(vm);
        p2.addStage(sDouble);
        p2.run({});
        CHECK(p2.sink().empty());
    }

    // ---- everything filtered out: sink stays empty (short-circuits the rest of the pipeline) ----
    {
        Pipeline p3(vm);
        p3.addStage(compile("Function(x): return None\n"));
        p3.addStage(compile("Function(x): return x + 1000\n"));   // never runs
        p3.run({val(vm, int64_t{1}).handle(), val(vm, int64_t{2}).handle()});
        CHECK(p3.sink().empty());
    }

    // ---- a stage that returns None-then-value mid-run keeps the pipeline going per-item ----
    {
        Pipeline p4(vm);
        p4.addStage(compile(R"KI(
Function(x):
    if x == 5:
        return None
    return x
)KI"));
        std::vector<Handle> in;
        for (int64_t i = 0; i < 10; ++i) in.push_back(val(vm, i).handle());
        p4.run(in);
        CHECK(p4.sink().size() == 9);   // 0..9 minus 5
    }

    // ---- adversarial: a stage that throws propagates cleanly ----
    {
        Pipeline p5(vm);
        p5.addStage(compile("Function(x): throw \"pipeline burst\"\n"));
        CHECK_THROWS(p5.run({val(vm, int64_t{1}).handle()}));
    }

    // ---- deep pipeline (many stages, each identity): output should equal input untouched ----
    {
        Pipeline p6(vm);
        Handle identity = compile("Function(x): return x\n");
        for (int i = 0; i < 20; ++i) p6.addStage(identity);
        std::vector<Handle> in;
        for (int64_t i = 0; i < 5; ++i) in.push_back(val(vm, i).handle());
        p6.run(in);
        CHECK(p6.sink().size() == 5);
        for (std::size_t i = 0; i < 5; ++i)
            CHECK(Value(vm, p6.sink()[i]).asInt("") == static_cast<int64_t>(i));
    }

    return RUN_TESTS();
}
