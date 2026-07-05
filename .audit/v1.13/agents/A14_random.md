# A14 — `random` module audit (stdlib_random.hpp)

Scope: `src/kirito/stdlib_random.hpp` (object-based RNG, `std::variant<fum::xoshiro256, std::mt19937_64>`),
supported by `src/fum/xoshiro256.hpp`. Existing tests: `tools/tests/unit/test_random_net_deep.cpp`,
`tools/tests/scripts/{spec_random,audit_random,random_generators,random_choices,random_prng_quality,r6_random}.ki`.
Method READ-ONLY, static reasoning (no build/run).

## Surface enumerated
Constructor `Random(seed=None, generator="xoshiro")` (aliases `xoshiro256`/`mt19937_64`; unknown → throw).
Attr `.generator` (read-only). Methods: `seed(a)`, `random()`, `uniform(a,b)`, `gauss(mu=0,sigma=1)` /
`normalvariate` (alias), `expovariate(lambd=1)`, `randint(a,b)` inclusive, `randrange(stop | start,stop[,step])`,
`choice(seq)`, `choices(population,k=1)`, `shuffle(seq)` (List only), `sample(population,k)`,
`_getstate_`/`_setstate_` (serialize/dump, `"<kind>:<engine-stream>"`).

## Coverage assessment (overall: STRONG)
The six `.ki` scripts + `test_random_net_deep.cpp` cover, for BOTH engines where relevant: seed determinism,
reseed, serialize/dump round-trip resuming the exact stream + preserving engine kind, range invariants,
randint both-bounds/`a==a`/`a>b`-throws, randrange stop/start,stop/step forms + **negative bounds**
(`randrange(-10,-5)`) + **negative step** (`randrange(10,0,-2)`) + empty-range/step=0 throws + keyword forms,
choice membership + scalar-not-list, choices shape/with-replacement/k-default/k=0/neg-k/huge-k/non-iterable/
empty/keyword, sample distinct/k=0/k=len/k>len-throws/neg-k-throws, shuffle permutation + rejects String,
gauss/normalvariate-alias-stream/expovariate non-negative + lambda<=0 throws, and a full statistical PRNG-quality
battery (uniformity, chi-square, monobit, per-bit, byte, runs, lag/autocorrelation, gauss/expo moments, unbiased
shuffle, choice uniformity). Coverage is genuinely comprehensive; the findings below are the residual edges.

---

### A14-1: `gauss`/`normalvariate` keyword-only argument silently loses its documented default
- severity: **Low-Medium**
- location: stdlib_random.hpp:142-150 (`gauss`/`normalvariate` impl); interacts with makeMethod hole-fill native.hpp:187-199
- category: bug / correctness (default handling)
- description: `gauss` and `normalvariate` advertise defaults `mu=0, sigma=1` (CLAUDE.md; impl reads
  `a.size()>0 ? asNum(a[0]) : 0.0`). But these methods are bound via `makeMethod` with **no registered
  defaults**, and makeMethod fills a *hole before the last supplied arg* with `None` (native.hpp:198). So
  `gauss(sigma=0.5)` reaches the impl as `[None, 0.5]`, and `asNum(vm, a[0])` on `None` throws
  `"expected a number"` (line 79-82) instead of using `mu=0`. The default only works for OMITTED trailing
  positionals, never for a keyword that skips a leading optional. (Python's `gauss(sigma=0.5)` works.)
- failure-scenario: `random.Random(1).gauss(sigma = 2.0)` → `KiritoError: expected a number` (should be a
  draw with mu=0, sigma=2). Same for `normalvariate(sigma=...)`.
- proposed-test: `assert isinstance(random.Random(1).gauss(sigma = 2.0), Float)` and
  `random.Random(1).gauss(mu=0.0, sigma=1.0) == random.Random(1).gauss(sigma=1.0)`.
- proposed-fix: have the impl treat a `None` slot as "use default" (mirror `randrange`'s `optInt` idiom:
  `mu = (a.size()>0 && !isNone(a[0])) ? asNum(a[0]) : 0.0`), or register real signatured defaults.
- confidence: **High** (code path is clear; `randrange` already uses the None-tolerant idiom, gauss does not).

### A14-2: `random()` / `uniform()` can return the upper bound (1.0 / b), violating the `[0,1)` contract
- severity: Low
- location: stdlib_random.hpp:126-141 (`random`, `uniform` via `std::uniform_real_distribution<double>`)
- category: bug / correctness (boundary)
- description: `random()` promises `[0.0, 1.0)` (docs, and the tests assert `x < 1.0`). It is implemented with
  `std::uniform_real_distribution<double>(0.0,1.0)`, which is built on `generate_canonical` — a facility that,
  by a long-standing libstdc++/libc++ defect, can round UP to exactly `1.0` for certain engine outputs. The
  loop tests (a few thousand draws) cannot disprove this; the contract is not actually guaranteed. Same for
  `uniform(a,b)` returning exactly `b` (docs say inclusive `[a,b]` for uniform, so that one is fine).
- failure-scenario: a downstream `Integer(r.random() * 10.0)` producing index `10` (the PRNG-quality test
  even defensively clamps `if b == 10: b = 9`, line 91-93 of random_prng_quality.ki — implicit acknowledgment).
- proposed-test: hard to force deterministically; document the contract as `[0,1]`-in-practice or add a
  post-clamp. A targeted test would need a crafted engine state.
- proposed-fix: if a strict half-open guarantee is wanted, reject/replace a drawn `1.0` (`while ((x=dist(e))>=1.0);`)
  or use `x = e_bits * 2^-53`. Otherwise adjust the documented contract.
- confidence: Medium (platform/stdlib-dependent; real but rare).

### A14-3: `gauss`/`normalvariate` do not validate `sigma` (negative / zero) — precondition `0 < stddev`
- severity: Low
- location: stdlib_random.hpp:142-150
- category: robustness / input validation
- description: `std::normal_distribution<double>(mu, sigma)` has the standard precondition `0 < stddev`.
  A negative `sigma` is passed straight through; `sigma == 0` too. libstdc++ won't crash (it computes
  `mu + sigma*z`, so negative sigma just mirrors, zero collapses to `mu`), but there is no clean error and no
  test. `expovariate` correctly validates `lambda > 0` (line 154) — gauss is the asymmetric gap.
- failure-scenario: `random.Random(1).gauss(0.0, -1.0)` silently returns a mirrored-normal draw; `gauss(0.0, 0.0)`
  always returns `0.0`. No diagnostic.
- proposed-test: `assert throws(Function(): return random.Random(1).gauss(0.0, -1.0))` (once a guard is added).
- proposed-fix: `if (sigma < 0.0) throw KiritoError("gauss: sigma must be non-negative");` (match Python leniency
  on 0, or reject `<= 0` for parity with expovariate — pick one and document).
- confidence: High that it is unvalidated; Medium on whether a guard is desired (Python is lenient).

### A14-4: `uniform(a, b)` with `a > b` is not validated — `std::uniform_real_distribution` precondition `a <= b`
- severity: Low
- location: stdlib_random.hpp:133-141
- category: robustness / input validation
- description: The standard requires `a <= b` for `uniform_real_distribution(a,b)`. `uniform(10.0, 5.0)` violates
  this; libstdc++ produces a value in the reversed interval (benign in practice, technically UB per the standard).
  Untested. (Python's `uniform` explicitly allows `a > b`, so leniency is defensible — but then it should be
  intentional/tested, not incidental.)
- failure-scenario: `random.Random(1).uniform(10.0, 5.0)` → value in `[5,10]` with no guarantee across stdlibs.
- proposed-test: assert the swapped call returns a value within `[min,max]`, documenting the intended behavior.
- proposed-fix: either normalize (`if (lo>hi) std::swap(lo,hi);`) for well-defined behavior, or document/test the
  reliance on lenient stdlib behavior.
- confidence: Medium.

### A14-5: `Random()` with no seed (system-random default path) is never exercised
- severity: Low
- location: stdlib_random.hpp:62-65 (`systemSeed`), 328-330 (constructor None-seed branch)
- category: coverage gap
- description: Every test passes an explicit seed (necessary for golden determinism). The `systemSeed()` path
  (`std::random_device`, `(rd()<<32)^rd()`) and the `args[0].isNone() → systemSeed()` branch are untested. A
  non-golden smoke check would still catch a regression (e.g. a throw or a type error).
- failure-scenario: a future refactor breaks default construction; no test notices.
- proposed-test: `assert isinstance(random.Random().random(), Float)` and
  `assert random.Random().random() != random.Random().random()` (near-certain).
- proposed-fix: add the two smoke asserts to an existing script.
- confidence: High (grep-confirmed no seedless construction in any random test).

### A14-6: Full-width / huge `randint` range untested (`uniform_int_distribution<int64_t>` full-range edge)
- severity: Low
- location: stdlib_random.hpp:160-169
- category: coverage gap / potential edge bug
- description: `randint` builds `std::uniform_int_distribution<int64_t>(lo, hi)`. The maximal case
  `randint(INT64_MIN, INT64_MAX)` makes the internal unsigned range `2^64-1` (the +1 wraps to 0) — a code path
  modern libstdc++ special-cases correctly, but it is untested here and is the classic off-by-one/overflow
  trap. Also `int64_t` as an `IntType` template argument is technically outside the standard's listed set
  (`short/int/long/long long`+unsigned) though fine on all mainstream platforms.
- failure-scenario: `random.Random(1).randint(-9223372036854775808, 9223372036854775807)` — verify it returns a
  valid in-range int and does not loop/UB.
- proposed-test: bounds-check a few draws of the full-range and a wide range like `randint(-10**18, 10**18)`.
- proposed-fix: none if libstdc++ handles it (it does); add the test to lock it.
- confidence: Medium (likely correct, but unverified — the mission explicitly flags this).

### A14-7: `Random` constructor seed edge cases untested (negative seed, non-integer seed rejection)
- severity: Low
- location: stdlib_random.hpp:328-330 (`args[0].asInt("Random seed")`, cast to `uint64_t`)
- category: coverage gap
- description: Negative seeds (`Random(-1)` → `uint64_t(0xFFFF...)`) and non-int seeds (`Random(3.5)`,
  `Random("x")` → should throw via `asInt`) are untested. Determinism of a negative seed and the type-error on a
  float/string seed are both unasserted.
- proposed-test: `assert random.Random(-1).random() == random.Random(-1).random()`;
  `assert throws(Function(): return random.Random(3.5))`.
- proposed-fix: add asserts.
- confidence: High.

### A14-8: `choices` has no `weights` parameter (weighted sampling not implemented)
- severity: Informational / scope
- location: stdlib_random.hpp:206-227
- category: gap vs. mission expectation (NOT a bug)
- description: The audit brief lists `choices(population, k=1, weights?)`. The implementation supports only
  `(population, k)` — no `weights`/`cum_weights`. CLAUDE.md likewise documents only `choices(population, k=1)`,
  so this is a deliberate scope boundary, not a defect. Noted so the expectation gap is explicit: Kirito's
  `choices` is uniform-only, unlike Python's weighted `random.choices`.
- proposed-fix: none required; if weighted sampling is ever added, it would live here.
- confidence: High.

### A14-9: keyword-only leading required arg yields an opaque numeric error instead of a "missing argument" diagnostic
- severity: Low
- location: stdlib_random.hpp:133-141 (`uniform`), 160-169 (`randint`) + makeMethod None-fill (native.hpp:198)
- category: diagnostics / usability
- description: Because these methods are bound with `minArgs=0`, a call like `uniform(b=5)` or `randint(b=5)`
  reaches the impl as `[None, 5]`; the `a.size()<2` guard is bypassed (size is 2 after None-fill), so it fails
  later at `asNum/asInt(None)` with `"expected a number"` / a type error rather than a clear
  `"uniform expects (a, b)"` / "missing required argument 'a'". Functionally safe (throws, no crash) but the
  message is misleading.
- proposed-test: `try: random.Random(1).uniform(b = 5) ... assert "a" in e or "expects" in e`.
- proposed-fix: pass a proper `minArgs` to `makeMethod` for the fixed-arity methods (2 for uniform/randint), so
  makeMethod raises the precise missing-arg error (native.hpp:195-197).
- confidence: High.

### A14-10: DRY — the `std::visit` distribution-dispatch idiom is duplicated ~9 times
- severity: Low (maintainability)
- location: stdlib_random.hpp:88-92, 128-130, 137-139, 146-148, 155-157, 165-167, 190-194, 235-239, 256-261
- category: DRY / cleanliness
- description: Every method repeats `std::visit([captures](auto& e){ return SomeDist(...)(e); }, engine)`. Only
  `pickOne` is factored (shared by `choice`/`choices`). A single templated helper
  `template<class D> auto draw(D dist){ return std::visit([&](auto& e){ return dist(e); }, engine); }` would
  collapse the boilerplate and centralize the variant handling. Not a bug; the current form is a legitimate,
  readable idiom.
- proposed-fix: add a `draw()` helper on `RandomState`.
- confidence: High (observation, not a defect).

### A14-11: `_setstate_` does not validate the engine-state string beyond the first tokens (adversarial/mismatched state untested)
- severity: Low
- location: stdlib_random.hpp:280-304
- category: robustness / coverage gap
- description: `_setstate_` splits on the first `:`, parses the tag, then `is >> e`. Trailing garbage after the
  parsed words is ignored; a cross-kind forgery (`"xoshiro:<mt-style words>"`) would be parsed as 4 xoshiro
  words without complaint. serialized blobs are effectively trusted, so severity is low, but the malformed-input
  behavior (missing prefix → throws; bad numbers → throws; empty payload; all-zero xoshiro state → operator>>
  sets failbit → throws) is only partially exercised. The happy-path round-trip is well tested; the reject
  paths for a hand-crafted bad String are not.
- failure-scenario: `var r = random.Random(0); r._setstate_("bogus")` — verify it throws the "missing kind
  prefix" error; `r._setstate_("xoshiro:0 0 0 0")` — verify all-zero rejection.
- proposed-test: assert both malformed-state calls throw with the expected messages.
- proposed-fix: none functionally; add the negative tests (and optionally check the stream fully consumed).
- confidence: Medium.

### A14-12 (verified-clean): GC-safety of result-list construction
- severity: none (positive finding)
- location: stdlib_random.hpp:198-266
- category: memory / GC safety
- description: Reviewed for dangling-handle hazards. `choices` roots the entire iterated `pool` and every picked
  element in a `RootScope` before the sole `vm.alloc` (lines 220-226). `sample` roots each picked element as it
  is placed into `out`, and the only Kirito allocation is the final `vm.alloc(std::move(out))` after all picks
  (lines 253-265) — no allocation occurs mid-loop, so the un-rooted `pool` tail cannot be swept. `choice`/
  `pickOne` return a `pool` element with no intervening allocation before the VM roots the result. `shuffle` is
  pure in-place swaps. No GC-safety defect found. (Also verified: the `bind(std::string(name).c_str(), ...)`
  idiom at line 143 is safe — the temporary `std::string` outlives the `makeMethod` copy within the
  full-expression.)
- confidence: High.

---

## Summary
- **Findings: 11** (1 low-med bug, 3 low bugs/robustness, 5 coverage gaps, 1 DRY, 1 scope note) + 1 verified-clean.
- **Confirmed bug: A14-1** — `gauss(sigma=…)` keyword-only call throws `"expected a number"` because
  makeMethod None-fills the skipped `mu` slot and the impl (unlike `randrange`) does not treat None as default.
- Coverage is otherwise **strong**: negative bounds, negative step, k=len, k>len, empty ranges, both engines,
  serialize+dump exact-stream resume, engine-kind preservation, and a full statistical quality battery are all
  tested.

### Top 5
1. **A14-1** (Low-Med, bug): `gauss`/`normalvariate` keyword-only `sigma` loses default → `"expected a number"`.
2. **A14-2** (Low, bug): `random()` can return `1.0` (uniform_real_distribution rounding) vs the `[0,1)` contract.
3. **A14-5** (Low, gap): seedless `Random()` / `systemSeed()` default path never tested.
4. **A14-3** (Low, robustness): `gauss` `sigma < 0`/`= 0` unvalidated (expovariate validates lambda; gauss doesn't).
5. **A14-6** (Low, gap): full-width/huge `randint` range (`INT64_MIN..INT64_MAX`) untested edge.
