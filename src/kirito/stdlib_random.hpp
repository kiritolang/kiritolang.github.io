#ifndef KIRITO_STDLIB_RANDOM_HPP
#define KIRITO_STDLIB_RANDOM_HPP

#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "builtins.hpp"
#include "collections.hpp"
#include "fum/xoshiro256.hpp"
#include "native.hpp"

namespace kirito {

// The native-binding idiom below re-uses `vm`/`self` as bound-method lambda parameters that
// intentionally shadow the enclosing getAttr/setup `vm`/`self` (same VM, by design). Silence
// -Wshadow for these mechanical bindings; it stays active in the evaluator/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

// A random generator object. There is no global RNG: you create a Random (default seed from the
// system, or an explicit seed for reproducibility) and call methods on it. Two engines are
// supported behind one uniform surface, chosen at construction time via `generator="xoshiro"` /
// `generator="mersenne_twister"`:
//
//   * "xoshiro"          — fum::xoshiro256++ (Blackman-Vigna, 2019). 256-bit state, period 2^256-1,
//                          ~1.5-1.75x faster than Mersenne Twister on raw operator() and every
//                          <random> distribution. The DEFAULT.
//   * "mersenne_twister" — std::mt19937_64. 19937-bit state, period 2^19937-1. Slower, longer
//                          period; kept for anyone who wants the historical `std` engine.
//
// Both satisfy `UniformRandomBitGenerator`, so every std::*_distribution accepts either engine
// verbatim — the class dispatches through a std::variant with std::visit.
class RandomState : public NativeClass<RandomState> {
public:
    static constexpr const char* kTypeName = "Random";

    // Engine variant: index 0 = xoshiro (the default), index 1 = Mersenne Twister. The order is
    // deliberate — a default-constructed Engine picks the DEFAULT engine.
    using Engine = std::variant<fum::xoshiro256, std::mt19937_64>;
    Engine engine;

    // Default constructor — used by the deserializer factory before `_setstate_` replaces the
    // engine anyway; picks the default (xoshiro) with a system-random seed.
    RandomState() : engine(fum::xoshiro256(systemSeed())) {}
    RandomState(uint64_t seed, bool useXoshiro)
        : engine(useXoshiro ? Engine(fum::xoshiro256(seed))
                            : Engine(std::mt19937_64(seed))) {}

    // The label reported by `.generator` and used as the serialization prefix.
    const char* generatorName() const {
        return engine.index() == 0 ? "xoshiro" : "mersenne_twister";
    }

    static uint64_t systemSeed() {
        std::random_device rd;
        return (static_cast<uint64_t>(rd()) << 32) ^ rd();
    }

    // Parse a `generator=` kwarg into a boolean (true = xoshiro). Accepts the canonical spellings
    // "xoshiro" / "mersenne_twister" plus the underlying-engine names "xoshiro256" / "mt19937_64",
    // so callers who know the specific engine can name it. Anything else throws.
    static bool parseGenerator(const std::string& name) {
        if (name == "xoshiro" || name == "xoshiro256") return true;
        if (name == "mersenne_twister" || name == "mt19937_64") return false;
        throw KiritoError("Random: unknown generator '" + name +
                          "' (expected 'xoshiro' or 'mersenne_twister')");
    }

    static int64_t asInt(KiritoVM& vm, Handle h) { return argInt(vm, h, "argument"); }
    static double asNum(KiritoVM& vm, Handle h) {
        const Object& o = vm.arena().deref(h);
        if (o.kind() == ValueKind::Integer) return static_cast<double>(static_cast<const IntVal&>(o).value());
        if (o.kind() == ValueKind::Float) return static_cast<const FloatVal&>(o).value();
        throw KiritoError("expected a number");
    }

    // Draw one element uniformly from `pool` WITH replacement — the shared primitive behind `choice`
    // (the k = 1 case, unwrapped to the element) and `choices` (k draws collected into a List).
    static Handle pickOne(RandomState& st, const std::vector<Handle>& pool) {
        std::size_t idx = std::visit(
            [&pool](auto& e) {
                return std::uniform_int_distribution<std::size_t>(0, pool.size() - 1)(e);
            },
            st.engine);
        return pool[idx];
    }

    std::vector<std::string> inspectMembers() const override {
        return {
            "generator: String",
            "random() -> Float", "uniform(a, b) -> Float", "randint(a, b) -> Integer",
            "randrange(start, stop, step) -> Integer", "choice(seq)", "choices(population, k) -> List",
            "shuffle(seq)",
            "sample(population, k) -> List", "seed(a)", "gauss(mu, sigma) -> Float",
            "normalvariate(mu, sigma) -> Float", "expovariate(lambd) -> Float",
        };
    }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
            return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
        };
        auto rng = [](KiritoVM& vm, Handle self) -> RandomState& {
            return static_cast<RandomState&>(vm.arena().deref(self));
        };
        // Non-method attribute: which engine is active. Read-only from Kirito (assignment would
        // hit the base setAttr which rejects it, matching the other native modules).
        if (name == "generator")
            return vm.makeString(rng(vm, self).generatorName());
        if (name == "seed")
            return bind("seed", {"a"}, [self, rng](KiritoVM& vm, std::span<const Handle> a) -> Handle {  // param name matches inspect "seed(a)"
                if (a.empty()) throw KiritoError("seed expects a value");
                uint64_t s = static_cast<uint64_t>(asInt(vm, a[0]));
                // Re-seed the CURRENT engine (do not switch kind).
                std::visit([s](auto& e) { e.seed(s); }, rng(vm, self).engine);
                return vm.none();
            });
        if (name == "random")
            return bind("random", {}, [self, rng](KiritoVM& vm, std::span<const Handle>) -> Handle {
                double x = std::visit(
                    [](auto& e) { return std::uniform_real_distribution<double>(0.0, 1.0)(e); },
                    rng(vm, self).engine);
                return vm.makeFloat(x);
            });
        if (name == "uniform")
            return bind("uniform", {"a", "b"}, [self, rng](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                if (a.size() < 2) throw KiritoError("uniform expects (a, b)");
                double lo = asNum(vm, a[0]), hi = asNum(vm, a[1]);
                double x = std::visit(
                    [lo, hi](auto& e) { return std::uniform_real_distribution<double>(lo, hi)(e); },
                    rng(vm, self).engine);
                return vm.makeFloat(x);
            });
        if (name == "gauss" || name == "normalvariate")
            return bind(std::string(name).c_str(), {"mu", "sigma"}, [self, rng](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                double mu = a.size() > 0 ? asNum(vm, a[0]) : 0.0;
                double sigma = a.size() > 1 ? asNum(vm, a[1]) : 1.0;
                double x = std::visit(
                    [mu, sigma](auto& e) { return std::normal_distribution<double>(mu, sigma)(e); },
                    rng(vm, self).engine);
                return vm.makeFloat(x);
            });
        if (name == "expovariate")
            return bind("expovariate", {"lambd"}, [self, rng](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                double lambda = a.size() > 0 ? asNum(vm, a[0]) : 1.0;
                if (lambda <= 0.0) throw KiritoError("expovariate: lambda must be positive");
                double x = std::visit(
                    [lambda](auto& e) { return std::exponential_distribution<double>(lambda)(e); },
                    rng(vm, self).engine);
                return vm.makeFloat(x);
            });
        if (name == "randint")  // inclusive [a, b]
            return bind("randint", {"a", "b"}, [self, rng](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                if (a.size() < 2) throw KiritoError("randint expects (a, b)");
                int64_t lo = asInt(vm, a[0]), hi = asInt(vm, a[1]);
                if (lo > hi) throw KiritoError("randint: empty range");
                int64_t v = std::visit(
                    [lo, hi](auto& e) { return std::uniform_int_distribution<int64_t>(lo, hi)(e); },
                    rng(vm, self).engine);
                return vm.makeInt(v);
            });
        if (name == "randrange")  // randrange(stop) | randrange(start, stop[, step]) — like range
            return bind("randrange", {"start", "stop", "step"}, [self, rng](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                auto optInt = [&](Handle h, int64_t dflt) -> int64_t {
                    return vm.arena().deref(h).kind() == ValueKind::None ? dflt : asInt(vm, h);
                };
                int64_t start = 0, stop = 0, step = 1;
                if (a.size() == 1) stop = asInt(vm, a[0]);                       // randrange(stop)
                else if (a.size() == 2) { start = optInt(a[0], 0); stop = asInt(vm, a[1]); }
                else if (a.size() == 3) { start = optInt(a[0], 0); stop = asInt(vm, a[1]); step = optInt(a[2], 1); }
                else throw KiritoError("randrange expects 1 to 3 arguments");
                if (step == 0) throw KiritoError("randrange: step must not be zero");
                // Count the members of range(start, stop, step), overflow-safe: stop-start can exceed
                // int64 for a wide span (signed subtraction would be UB), so work in unsigned wraparound.
                bool empty = step > 0 ? !(stop > start) : !(start > stop);
                if (empty) throw KiritoError("randrange: empty range");
                uint64_t span = step > 0 ? static_cast<uint64_t>(stop) - static_cast<uint64_t>(start)
                                         : static_cast<uint64_t>(start) - static_cast<uint64_t>(stop);
                uint64_t ustep = step > 0 ? static_cast<uint64_t>(step) : (0ull - static_cast<uint64_t>(step));
                uint64_t count = (span + ustep - 1) / ustep;   // number of members
                if (count > static_cast<uint64_t>(INT64_MAX)) throw KiritoError("randrange: range too large to sample");
                int64_t k = std::visit(
                    [count](auto& e) {
                        return std::uniform_int_distribution<int64_t>(0, static_cast<int64_t>(count) - 1)(e);
                    },
                    rng(vm, self).engine);
                // start + k*step, overflow-safe; the result is in range by construction so it fits int64
                return vm.makeInt(static_cast<int64_t>(static_cast<uint64_t>(start) + static_cast<uint64_t>(k) * static_cast<uint64_t>(step)));
            });
        if (name == "choice")   // one random element (scalar) — the k = 1 case of choices, unwrapped
            return bind("choice", {"seq"}, [self, rng](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                if (a.empty()) throw KiritoError("choice expects a sequence");
                auto items = vm.arena().deref(a[0]).iterate(vm);
                if (!items) throw KiritoError("choice expects an iterable");
                if (items.value().empty()) throw KiritoError("choice from empty sequence");
                return pickOne(rng(vm, self), items.value());
            });
        if (name == "choices")  // k random elements WITH replacement -> new List (Python random.choices)
            return bind("choices", {"population", "k"}, [self, rng](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                if (a.empty()) throw KiritoError("choices expects a population");
                auto items = vm.arena().deref(a[0]).iterate(vm);
                if (!items) throw KiritoError("choices expects an iterable population");
                const std::vector<Handle>& pool = items.value();
                if (pool.empty()) throw KiritoError("choices from empty population");
                // k defaults to 1 (like choice, but as a 1-element List); an explicit None is also 1.
                int64_t k = (a.size() < 2 || vm.arena().deref(a[1]).kind() == ValueKind::None) ? 1 : asInt(vm, a[1]);
                if (k < 0) throw KiritoError("choices: k must be non-negative");
                // Resource guard on the result length, matching runtime.hpp's kMaxRepeat for list
                // repetition (not visible here — stdlib_random is included before that constant).
                if (static_cast<uint64_t>(k) > 256ull * 1024 * 1024)
                    throw KiritoError("choices: k too large");
                RootScope rs(vm);
                for (Handle h : pool) rs.add(h);   // keep the (possibly freshly-iterated) pool alive
                auto out = std::make_unique<ListVal>();
                out->elems.reserve(static_cast<std::size_t>(k));
                for (int64_t i = 0; i < k; ++i)
                    out->elems.push_back(rs.add(pickOne(rng(vm, self), pool)));
                return vm.alloc(std::move(out));
            });
        if (name == "shuffle")  // in place, requires a List
            return bind("shuffle", {"seq"}, [self, rng](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                if (a.empty()) throw KiritoError("shuffle requires a List");
                Object& o = vm.arena().deref(a[0]);
                if (o.kind() != ValueKind::List) throw KiritoError("shuffle requires a List");
                auto& e = static_cast<ListVal&>(o).elems;
                for (std::size_t i = e.size(); i > 1; --i) {
                    std::size_t j = std::visit(
                        [i](auto& en) {
                            return std::uniform_int_distribution<std::size_t>(0, i - 1)(en);
                        },
                        rng(vm, self).engine);
                    std::swap(e[i - 1], e[j]);
                }
                return vm.none();
            });
        if (name == "sample")  // k unique elements -> new List
            return bind("sample", {"population", "k"}, [self, rng](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                if (a.size() < 2) throw KiritoError("sample expects (population, k)");
                auto items = vm.arena().deref(a[0]).iterate(vm);
                if (!items) throw KiritoError("sample expects an iterable population");
                std::vector<Handle> pool = items.value();
                int64_t k = asInt(vm, a[1]);
                if (k < 0 || static_cast<std::size_t>(k) > pool.size())
                    throw KiritoError("sample: k out of range");
                RootScope rs(vm);
                auto out = std::make_unique<ListVal>();
                for (int64_t picked = 0; picked < k; ++picked) {
                    std::size_t j = std::visit(
                        [picked, &pool](auto& e) {
                            return std::uniform_int_distribution<std::size_t>(
                                static_cast<std::size_t>(picked), pool.size() - 1)(e);
                        },
                        rng(vm, self).engine);
                    std::swap(pool[picked], pool[j]);
                    out->elems.push_back(rs.add(pool[picked]));
                }
                return vm.alloc(std::move(out));
            });
        // --- serialization (serialize / dump): the full engine state, tagged with the generator
        // kind, so a restored generator continues the exact same stream on the same engine
        // (reproducible checkpoints). The standard/xoshiro engine's stream operators emit/parse
        // its complete internal state. Format: "<kind>:<engine-stream>" (e.g. "xoshiro:...",
        // "mersenne_twister:..."). ---
        if (name == "_getstate_")
            return bind("_getstate_", {}, [self, rng](KiritoVM& vm, std::span<const Handle>) -> Handle {
                RandomState& st = rng(vm, self);
                std::ostringstream os;
                os << st.generatorName() << ':';
                std::visit([&os](auto& e) { os << e; }, st.engine);
                return vm.makeString(os.str());
            });
        if (name == "_setstate_")
            return bind("_setstate_", {"state"}, [self, rng](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                const Object& o = vm.arena().deref(a[0]);
                if (o.kind() != ValueKind::String)
                    throw KiritoError("Random _setstate_: expected the engine-state String");
                const std::string& s = static_cast<const StrVal&>(o).value();
                auto colon = s.find(':');
                if (colon == std::string::npos)
                    throw KiritoError("Random _setstate_: malformed engine state (missing kind prefix)");
                std::string kind = s.substr(0, colon);
                std::istringstream is(s.substr(colon + 1));
                RandomState& st = rng(vm, self);
                if (kind == "xoshiro" || kind == "xoshiro256") {
                    fum::xoshiro256 e;
                    if (!(is >> e)) throw KiritoError("Random _setstate_: malformed xoshiro state");
                    st.engine = e;
                } else if (kind == "mersenne_twister" || kind == "mt19937_64") {
                    std::mt19937_64 e;
                    if (!(is >> e)) throw KiritoError("Random _setstate_: malformed mt19937_64 state");
                    st.engine = e;
                } else {
                    throw KiritoError("Random _setstate_: unknown generator '" + kind + "'");
                }
                return vm.none();
            });
        return Object::getAttr(vm, self, name);
    }
};

class RandomModule : public NativeModule {
public:
    std::string name() const override { return "random"; }
    void setup(ModuleBuilder& m) override {
        // Let serialize/dump reconstruct a Random: build a default one; _setstate_ replaces its
        // engine (with the correct kind derived from the state prefix) below.
        m.vm().registerDeserializer("Random", [](KiritoVM& vm, Handle) -> Handle {
            return vm.alloc(std::make_unique<RandomState>());
        });
        // Random(seed = None, generator = "xoshiro"). The kwarg lets callers pin the
        // Mersenne Twister when they want its longer period or exact-stream compatibility with
        // pre-existing checkpoints; the default (xoshiro) is faster.
        m.fn("Random",
             {{"seed", "", m.vm().none()},
              {"generator", "String", m.vm().makeString("xoshiro")}},
             "Random",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 Args args(vm, a, "Random");
                 bool useXoshiro = RandomState::parseGenerator(args[1].asString("generator"));
                 uint64_t seed = args[0].isNone()
                                     ? RandomState::systemSeed()
                                     : static_cast<uint64_t>(args[0].asInt("Random seed"));
                 return vm.alloc(std::make_unique<RandomState>(seed, useXoshiro));
             });
    }
};

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
