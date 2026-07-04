// embed_markov_text.cpp — a bigram Markov text model. C++ owns the training/generation control
// loop and the fixed RNG seed; Kirito owns the two policies (compiled from source):
//   build   : Function(corpus: String) -> Dict        — split the corpus, map each word to the
//                                                        ordered List of words that follow it.
//   generate: Function(model: Dict, start, n, rng) -> List of String
//                                                        — walk the chain, picking each next word
//                                                        with a SEEDED Random(7) so the output is
//                                                        deterministic.
//
// Flow: C++ (feed corpus) -> Kirito build -> C++ holds the model Dict -> Kirito generate (seeded)
// -> C++ verifies structural invariants (starts with `start`; every step is a legal edge).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// The corpus splitting lives INSIDE Kirito (.split()), and so does the whole bigram walk. C++ never
// tokenizes — it only drives and audits.
static const char* BUILD_SRC = R"KI(
Function(corpus : String) -> Dict:
    var words = corpus.split()
    var model = {}
    var i = 0
    while i < len(words) - 1:
        var w = words[i]
        var nxt = words[i + 1]
        if not (w in model):
            model[w] = []
        model[w].append(nxt)
        i = i + 1
    return model
)KI";

static const char* GEN_SRC = R"KI(
Function(model : Dict, start : String, n : Integer, rng) -> List:
    if not (start in model):
        throw "start word not in model: " + start
    var result = [start]
    var current = start
    while len(result) < n:
        var choices = model[current]
        if len(choices) == 0:
            break
        var nxt = rng.choice(choices)
        result.append(nxt)
        current = nxt
    return result
)KI";

class MarkovEngine {
public:
    MarkovEngine(KiritoVM& vm, Handle build, Handle gen) : vm_(vm), build_(build), gen_(gen) {}

    // corpus -> model Dict handle (the caller roots it).
    Handle train(const std::string& corpus) {
        RootScope rs(vm_);
        std::array<Handle, 1> args{rs.add(Value(vm_, corpus).handle())};
        return vm_.arena().deref(build_).call(vm_, args);
    }

    // (model, start, n, rng) -> List-of-String handle. rng is passed straight through as a Handle,
    // so a fixed Random(7) makes the walk deterministic.
    Handle generate(Handle model, const std::string& start, int64_t n, Handle rng) {
        RootScope rs(vm_);
        std::array<Handle, 4> args{
            model,
            rs.add(Value(vm_, start).handle()),
            rs.add(Value(vm_, n).handle()),
            rng,
        };
        return vm_.arena().deref(gen_).call(vm_, args);
    }

private:
    KiritoVM& vm_;
    Handle    build_;
    Handle    gen_;
};

// The follow-words of `word` as a plain vector<string> (empty if the word is a dead end / absent).
static std::vector<std::string> follows(Value model, const std::string& word) {
    std::vector<std::string> out;
    Dict m = model.asDict("model");
    if (auto lst = m.tryGet(word))
        for (Value w : lst->items())
            out.push_back(w.asStringRef("follow word"));
    return out;
}

// Is word -> next a legal transition in the model?
static bool legalEdge(Value model, const std::string& from, const std::string& to) {
    for (const std::string& w : follows(model, from))
        if (w == to) return true;
    return false;
}

int main() {
    KiritoVM vm;
    RootScope roots(vm);  // keep the compiled policies + rng + model alive across every call

    Handle buildH = roots.add(vm.runSource(BUILD_SRC));
    Handle genH   = roots.add(vm.runSource(GEN_SRC));
    MarkovEngine engine(vm, buildH, genH);

    // A fresh, seeded RNG. A brand-new Random(7) each time it is used gives a reproducible stream.
    auto freshRng = [&]() { return roots.add(vm.runSource("var random = import(\"random\")\nrandom.Random(7)")); };

    // ---- train ----
    // Bigrams of "the cat sat on the mat the cat ran":
    //   the->cat  cat->sat  sat->on  on->the  the->mat  mat->the  the->cat  cat->ran
    const std::string corpus = "the cat sat on the mat the cat ran";
    Handle modelH = roots.add(engine.train(corpus));
    Value  model(vm, modelH);

    // ---- structural checks on the learned transitions ----
    CHECK(model.isDict());
    {
        std::vector<std::string> theFollows = follows(model, "the");
        // "the" is followed (in order) by cat, mat, cat.
        CHECK(theFollows.size() == 3);
        int cat = 0, mat = 0;
        for (const std::string& w : theFollows) {
            if (w == "cat") ++cat;
            if (w == "mat") ++mat;
        }
        CHECK(cat == 2);
        CHECK(mat == 1);
        CHECK(legalEdge(model, "the", "cat"));
        CHECK(legalEdge(model, "the", "mat"));
        CHECK(!legalEdge(model, "the", "on"));
    }
    // A few more edges the builder must have captured.
    CHECK(legalEdge(model, "cat", "sat"));
    CHECK(legalEdge(model, "cat", "ran"));
    CHECK(legalEdge(model, "sat", "on"));
    CHECK(legalEdge(model, "on", "the"));
    CHECK(legalEdge(model, "mat", "the"));
    // "ran" is a terminal word (never appears before another) — a dead end, not a key.
    CHECK(follows(model, "ran").empty());

    // ---- generate: seeded, so deterministic; assert INVARIANTS, not exact text ----
    {
        Handle seqH = roots.add(engine.generate(modelH, "the", 6, freshRng()));
        Value  seq(vm, seqH);
        CHECK(seq.isList());
        std::vector<std::string> words;
        for (Value w : seq.items()) words.push_back(w.asStringRef("generated word"));

        CHECK(!words.empty());
        CHECK(words.size() <= 6);        // "up to n" words
        CHECK(words.front() == "the");   // starts with `start`
        // every consecutive pair is a legal edge in the model (unless we hit a dead end and stopped)
        for (std::size_t i = 0; i + 1 < words.size(); ++i)
            CHECK(legalEdge(model, words[i], words[i + 1]));
    }

    // ---- determinism: two independent Random(7) walks produce the SAME sequence ----
    {
        Value a(vm, roots.add(engine.generate(modelH, "the", 5, freshRng())));
        Value b(vm, roots.add(engine.generate(modelH, "the", 5, freshRng())));
        CHECK(a.len() == b.len());
        std::vector<Value> av = a.items(), bv = b.items();
        for (std::size_t i = 0; i < av.size(); ++i)
            CHECK(av[i].asStringRef("a") == bv[i].asStringRef("b"));
    }

    // ---- n == 1 yields exactly the start word (no transition taken) ----
    {
        Value one(vm, roots.add(engine.generate(modelH, "cat", 1, freshRng())));
        CHECK(one.isList());
        CHECK(one.len() == 1);
        CHECK(one.items().at(0).asStringRef("only word") == "cat");
    }

    // ---- terminal start: "ran" has no successors, so the walk stops at length 1 ----
    {
        Value dead(vm, roots.add(engine.generate(modelH, "ran", 4, freshRng())));
        CHECK(dead.len() == 1);
        CHECK(dead.items().at(0).asStringRef("dead word") == "ran");
    }

    // ---- adversarial: generating from a word that isn't in the model must throw ----
    CHECK_THROWS(engine.generate(modelH, "zzz-not-a-word", 5, freshRng()));

    return RUN_TESTS();
}
