// embed_job_scheduler.cpp — a priority job scheduler whose SCORING is Kirito. C++ owns the job
// queue (each job an id/name/deadline/weight struct) and the run loop; a pluggable Kirito
// Function(job: Dict) -> Integer|Float assigns each job a priority score. C++ sorts jobs by
// descending score and "runs" them, recording the run order. Swap the scoring policy and the run
// order changes.
//
// Flow per run: C++ (each job → Dict) → Kirito (scoring policy) → C++ (stable sort by score desc,
// record order).

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

struct Job {
    std::string name;
    int64_t     deadline;   // lower = more urgent
    double      weight;     // higher = more valuable
};

// A scheduler over a fixed job set: give it a Kirito scoring function, get back the run order.
class Scheduler {
public:
    explicit Scheduler(KiritoVM& vm) : vm_(vm) {}

    void add(const Job& j) { jobs_.push_back(j); }

    static Handle jobToDict(KiritoVM& vm, const Job& j) {
        Dict d(vm);
        d.set("name",     Value(vm, j.name));
        d.set("deadline", Value(vm, j.deadline));
        d.set("weight",   Value(vm, j.weight));
        return d.handle();
    }

    // Score every job through the Kirito policy, sort by descending score (stable, so ties keep
    // insertion order), and return the names in run order.
    std::vector<std::string> run(Handle scorer) {
        RootScope rs(vm_);
        struct Scored { std::size_t idx; double score; };
        std::vector<Scored> scored;
        scored.reserve(jobs_.size());
        for (std::size_t i = 0; i < jobs_.size(); ++i) {
            Handle jobH = rs.add(jobToDict(vm_, jobs_[i]));
            std::array<Handle, 1> args{jobH};
            Handle sH = rs.add(vm_.arena().deref(scorer).call(vm_, args));
            Value s(vm_, sH);
            // The policy MUST hand back a number. Anything else (None, String, ...) is a bug in
            // the policy — fail loudly rather than silently ordering by garbage.
            if (!s.isNumber())
                throw KiritoError("scheduler: score must be a number, got '" + s.typeName() + "'");
            scored.push_back({i, s.asFloat("score")});
        }
        std::stable_sort(scored.begin(), scored.end(),
                         [](const Scored& a, const Scored& b) { return a.score > b.score; });
        std::vector<std::string> order;
        order.reserve(scored.size());
        for (const Scored& s : scored) order.push_back(jobs_[s.idx].name);
        return order;
    }

private:
    KiritoVM&        vm_;
    std::vector<Job> jobs_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    Scheduler sched(vm);
    sched.add({"backup",  100, 1.0});
    sched.add({"report",   40, 3.0});
    sched.add({"deploy",   40, 6.0});   // same deadline as "report", higher weight
    sched.add({"cleanup",  10, 0.5});

    // Policy A — earliest-deadline-first: urgency is the negated deadline, so the soonest deadline
    // scores highest. Pure Integer arithmetic → Kirito unary minus.
    Handle edf = compile(R"KI(
Function(job) -> Integer:
    return -job["deadline"]
)KI");

    // Policy B — weight/deadline ratio: value per unit of time. Float division and comparison in
    // Kirito; a distant-but-valuable job can outrank an urgent-but-cheap one.
    Handle ratio = compile(R"KI(
Function(job) -> Float:
    var d = job["deadline"] / 1.0
    if d <= 0.0:
        return job["weight"] * 1000.0
    return job["weight"] / d
)KI");

    // ---- run order under earliest-deadline-first ----
    // deadlines: cleanup(10) < report(40) = deploy(40) < backup(100); ties keep insertion order,
    // so report precedes deploy.
    std::vector<std::string> orderEdf = sched.run(edf);
    CHECK(orderEdf.size() == 4);
    CHECK(orderEdf.at(0) == "cleanup");
    CHECK(orderEdf.at(1) == "report");
    CHECK(orderEdf.at(2) == "deploy");
    CHECK(orderEdf.at(3) == "backup");

    // ---- run order under weight/deadline ratio ----
    // ratios: deploy 6/40=0.150, report 3/40=0.075, cleanup 0.5/10=0.050, backup 1/100=0.010.
    std::vector<std::string> orderRatio = sched.run(ratio);
    CHECK(orderRatio.size() == 4);
    CHECK(orderRatio.at(0) == "deploy");
    CHECK(orderRatio.at(1) == "report");
    CHECK(orderRatio.at(2) == "cleanup");
    CHECK(orderRatio.at(3) == "backup");

    // ---- the two policies genuinely disagree ----
    CHECK(orderEdf != orderRatio);
    CHECK(orderEdf.at(0) != orderRatio.at(0));   // cleanup vs deploy at the head

    // ---- a scorer we can reason about numerically: deploy's ratio really is highest ----
    {
        Value fn(vm, ratio);
        Value deployScore = fn.call({ Value(vm, Scheduler::jobToDict(vm,
                                        Job{"deploy", 40, 6.0})) });
        Value backupScore = fn.call({ Value(vm, Scheduler::jobToDict(vm,
                                        Job{"backup", 100, 1.0})) });
        CHECK(deployScore.isFloat());
        CHECK(deployScore > backupScore);
        // 6/40 == 0.15 exactly at IEEE-754 for these operands.
        CHECK(deployScore.asFloat("deploy") == 0.15);
    }

    // ---- deadline-0 guard: the ratio policy special-cases a non-positive deadline ----
    {
        Scheduler urgent(vm);
        urgent.add({"now",   0, 2.0});   // deadline 0 → 2.0 * 1000
        urgent.add({"later", 5, 9.0});   // 9/5 = 1.8
        std::vector<std::string> ord = urgent.run(ratio);
        CHECK(ord.at(0) == "now");
        CHECK(ord.at(1) == "later");
    }

    // ---- single-job scheduler: order is trivially that job ----
    {
        Scheduler one(vm);
        one.add({"solo", 7, 4.0});
        std::vector<std::string> ord = one.run(edf);
        CHECK(ord.size() == 1);
        CHECK(ord.at(0) == "solo");
    }

    // ---- adversarial: a policy returning None must throw (isNumber rejects it) ----
    {
        Handle bad = compile("Function(job): return None\n");
        CHECK_THROWS(sched.run(bad));
    }

    // ---- adversarial: a policy returning a String must throw ----
    {
        Handle bad = compile("Function(job): return job[\"name\"]\n");
        CHECK_THROWS(sched.run(bad));
    }

    // ---- adversarial: a policy indexing a missing key throws out of Kirito ----
    {
        Handle bad = compile("Function(job): return job[\"missing\"]\n");
        CHECK_THROWS(sched.run(bad));
    }

    return RUN_TESTS();
}
