// embed_ga.cpp — a tiny genetic-algorithm framework. C++ owns the population, tournament
// selection, uniform crossover, and mutation; the FITNESS FUNCTION is Kirito. Every generation
// evaluates each individual by calling the Kirito fitness function; the C++ side then does
// selection + crossover + mutation with a seeded deterministic PRNG so the test always converges
// on the same answer.
//
// Classic "GA finds the target String" demo: individuals are String genomes of a fixed length,
// fitness = number of characters matching the target. The Kirito fitness function is a plain
// Function(individual: String) -> Integer; the target itself is a global var inside the Kirito
// scope. We verify convergence within a bounded number of generations, deterministic replay
// (same seed → same evolution), and that a bad fitness (returning the wrong type) throws.

#include <algorithm>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

class Ga {
public:
    Ga(KiritoVM& vm, Handle fitnessFn, std::size_t popSize, std::size_t genomeLen, uint32_t seed)
        : vm_(vm), fitness_(fitnessFn), rng_(seed), popSize_(popSize), genomeLen_(genomeLen) {
        pop_.reserve(popSize);
        for (std::size_t i = 0; i < popSize; ++i) pop_.push_back(randomGenome());
    }

    // Evolve for up to `maxGen` generations or until fitness == genomeLen. Returns the best
    // individual and its fitness at that point.
    std::pair<std::string, int64_t> evolve(std::size_t maxGen) {
        std::string best;
        int64_t bestFit = -1;
        for (std::size_t g = 0; g < maxGen; ++g) {
            std::vector<int64_t> fits;
            fits.reserve(pop_.size());
            RootScope rs(vm_);
            Handle fH = rs.add(fitness_);
            for (const auto& ind : pop_) {
                Handle sH = rs.add(vm_.makeString(ind));
                std::array<Handle, 1> args{sH};
                Handle outH = rs.add(vm_.arena().deref(fH).call(vm_, args));
                fits.push_back(Value(vm_, outH).asInt("fitness"));
            }
            for (std::size_t i = 0; i < pop_.size(); ++i) {
                if (fits[i] > bestFit) { bestFit = fits[i]; best = pop_[i]; }
            }
            if (bestFit == static_cast<int64_t>(genomeLen_)) return {best, bestFit};
            pop_ = nextGen(fits);
        }
        return {best, bestFit};
    }

private:
    std::string randomGenome() {
        std::string s(genomeLen_, ' ');
        std::uniform_int_distribution<int> d(32, 126);
        for (auto& c : s) c = static_cast<char>(d(rng_));
        return s;
    }
    std::size_t tournament(const std::vector<int64_t>& fits, std::size_t k) {
        std::uniform_int_distribution<std::size_t> pick(0, fits.size() - 1);
        std::size_t best = pick(rng_);
        for (std::size_t i = 1; i < k; ++i) {
            std::size_t c = pick(rng_);
            if (fits[c] > fits[best]) best = c;
        }
        return best;
    }
    std::string crossover(const std::string& a, const std::string& b) {
        std::string out(genomeLen_, ' ');
        std::uniform_int_distribution<int> coin(0, 1);
        for (std::size_t i = 0; i < genomeLen_; ++i) out[i] = coin(rng_) ? a[i] : b[i];
        return out;
    }
    std::string mutate(std::string s) {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        std::uniform_int_distribution<int> ch(32, 126);
        for (auto& c : s) if (u(rng_) < 0.03) c = static_cast<char>(ch(rng_));
        return s;
    }
    std::vector<std::string> nextGen(const std::vector<int64_t>& fits) {
        std::vector<std::string> next;
        next.reserve(pop_.size());
        // elitism: carry the single fittest over unchanged
        std::size_t eliteIdx = 0;
        for (std::size_t i = 1; i < pop_.size(); ++i) if (fits[i] > fits[eliteIdx]) eliteIdx = i;
        next.push_back(pop_[eliteIdx]);
        while (next.size() < pop_.size()) {
            std::size_t a = tournament(fits, 3);
            std::size_t b = tournament(fits, 3);
            next.push_back(mutate(crossover(pop_[a], pop_[b])));
        }
        return next;
    }

    KiritoVM& vm_;
    Handle fitness_;
    std::mt19937 rng_;
    std::size_t popSize_;
    std::size_t genomeLen_;
    std::vector<std::string> pop_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* s) { return vm.runSource(s); };

    // Kirito fitness: count matching characters against a target set in the Kirito scope.
    Handle fit = compile(R"KI(
var target = "HELLO"
Function(s):
    var n = 0
    var i = 0
    while i < len(s) and i < len(target):
        if s[i] == target[i]:
            n = n + 1
        i = i + 1
    return n
)KI");

    // --- convergence: with a fixed seed, GA reaches the exact target within 400 generations ---
    {
        Ga ga(vm, fit, /*pop*/ 80, /*len*/ 5, /*seed*/ 42);
        auto [best, bestFit] = ga.evolve(400);
        CHECK(bestFit == 5);
        CHECK(best == "HELLO");
    }

    // --- deterministic replay: same seed + same fitness -> same evolution ---
    {
        Ga a(vm, fit, 40, 5, 100);
        Ga b(vm, fit, 40, 5, 100);
        auto [ba, fa] = a.evolve(50);
        auto [bb, fb] = b.evolve(50);
        CHECK(fa == fb);
        CHECK(ba == bb);
    }

    // --- different seed -> (probably) different intermediate path but eventual convergence ---
    {
        Ga ga(vm, fit, 80, 5, 7);
        auto [best, bestFit] = ga.evolve(400);
        CHECK(bestFit == 5);
        CHECK(best == "HELLO");
    }

    // --- swap the Kirito fitness at runtime: MAXIMIZE String length via a different rule ---
    Handle fitLen = compile(R"KI(
Function(s):
    var n = 0
    for c in s:
        if c == "!":
            n = n + 1
    return n
)KI");
    {
        Ga ga(vm, fitLen, 60, 5, 3);
        auto [best, bestFit] = ga.evolve(200);
        CHECK(bestFit == 5);
        CHECK(best == "!!!!!");
    }

    // --- adversarial: a fitness function that returns the WRONG type throws cleanly ---
    Handle badFit = compile("Function(s): return \"not-an-integer\"\n");
    {
        Ga ga(vm, badFit, 20, 5, 1);
        CHECK_THROWS(ga.evolve(1));
    }

    // --- adversarial: a fitness function that THROWS inside Kirito propagates ---
    Handle throwingFit = compile("Function(s): throw \"fit is angry\"\n");
    {
        Ga ga(vm, throwingFit, 20, 5, 1);
        CHECK_THROWS(ga.evolve(1));
    }

    return RUN_TESTS();
}
