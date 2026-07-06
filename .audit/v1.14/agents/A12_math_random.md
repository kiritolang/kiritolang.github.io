# A12 — math + random audit (v1.14)

- **Agent:** A12
- **Subsystem:** `src/kirito/stdlib_math.hpp` (286 lines), `src/kirito/stdlib_random.hpp` (350 lines)
- **Method:** read-only source scan + live probes on `build-debug/ki` (p1–p7 in scratchpad).
- **Prior findings merged:** v1.13 `A13_math.md` (10 findings), `A14_random.md`. This pass hunts NEW angles.
- **Contract (CLAUDE.md):** math domain errors THROW (`sqrt(-1)`/`log(0)`/`asin(2)`/`acosh(0)`/
  `atanh(1)`/`gamma(0)`/`pow(-2,0.5)`/`fmod(x,0)`); NaN arg passes through; overflow→inf is NOT a
  domain error. Random: `Random(seed, generator="xoshiro"|"mersenne_twister")`, no global state;
  distributions; `choices` WITH replacement k=1 default; `sample` WITHOUT; serialize tags engine kind.

## Verdict

The module is **exceptionally well-hardened** — among the cleanest in the codebase. Every probe
confirmed correct behavior. **No Critical/High/Medium bug found.** Notably, the v1.13 A13-1 Medium
(`fmod(inf, finite)` silent NaN) **is now FIXED** — `stdlib_math.hpp:106-109` throws
`"fmod: math domain error (infinite dividend)"` and is regression-tested
(`test_audit_v113.cpp`, `verify_math.ki`). All findings below are Low / coverage / note.

Live-verified robust this pass:
- Domain throws: `log2/log10(-x)`, `log1p(-2)`, `gamma(-1)`, `fmod(inf,3)` all THROW; `expm1(1000)`→inf,
  `hypot(inf,0)`→inf, `atan2(0,0)`→0, `pow(0,0)`→1, `sqrt(-0.0)`→-0.0 all pass (correct range/values).
- INT64_MIN paths: `gcd(IMIN,IMIN)`/`gcd(IMIN,0)`/`lcm(IMIN,1)` THROW "too large"; `factorial(IMIN)`,
  `comb(IMIN,2)`, `perm(IMIN,2)` THROW non-negative — **no UB / no crash** (the `mag()` helpers work).
- Overflow: `factorial(25)`, `perm(21)`, `prod([1e6]*4)` all THROW; `comb(64,32)`=1832624140942590534
  (int128 widening keeps a representable result) OK.
- Random: xoshiro + mt19937_64 both deterministic for a fixed seed; engines differ; `randint(5,5)`=5,
  `randint(lo>hi)` throws; `randrange` empty/zero-step throw; `choice([])`/`sample(pop,k>len)`/
  `sample(k<0)`/`gauss(σ<0)`/`expovariate(λ≤0)`/`choices(k<0)`/`Random(generator="bogus")` all throw;
  full-range `randint(IMIN,IMAX)` OK; `seed()` resets the stream and keeps engine kind;
  `uniform(5,2)` reversed bounds returns value in [2,5] (no crash); `random()` stayed in [0,1) over 2e5 draws.
- Serialize/dump round-trip (both engines, text + binary) resumes the EXACT stream mid-sequence and
  preserves `.generator`. All four `_setstate_` error paths (missing kind prefix, unknown kind,
  malformed engine stream, non-String arg) throw clean messages.

---

## Findings

### A12-1: gauss/expovariate/uniform param validation is bypassed by a NaN argument (silent NaN out)
- **Severity:** Low
- **Location:** `stdlib_random.hpp:153` (`gauss`/`normalvariate` `sigma < 0.0`), `:162`
  (`expovariate` `lambda <= 0.0`), `:143` (`uniform`, no validation)
- **Category:** validation gap / policy inconsistency
- **Description:** The param guards use `<`/`<=` comparisons that a NaN silently defeats:
  `gauss(0.0, nan)` → `nan < 0.0` is false → passes → `std::normal_distribution(0, nan)` → returns
  `nan`; `expovariate(nan)` → `nan <= 0.0` false → returns `nan`; `uniform(nan, 1)` → `nan`. These
  return a quiet NaN rather than throwing. This is arguably consistent with the math module's
  "NaN passes through" policy, but it is INCONSISTENT with the surrounding strict param validation
  (a `sigma = -1` throws "sigma must be non-negative" but `sigma = nan` does not) and there is no
  documented rule that random-distribution params follow the NaN-passthrough convention.
- **Failure scenario:** a program computes `sigma` from data, gets a NaN (e.g. `0.0/0.0`), and
  `gauss` silently emits NaN samples instead of failing loudly — the exact silent-NaN outcome the
  math module was hardened against.
- **Proposed test:** decide the policy, then pin it: either `assert throws(Function(): return
  r.gauss(0.0, math.nan))` (strict) or `assert math.isnan(r.gauss(0.0, math.nan))` (passthrough).
- **Proposed fix:** if strict is desired, guard with `if (std::isnan(sigma) || sigma < 0.0)` (and the
  `lambda`/`uniform` analogues). If passthrough is desired, document it. Prefer strict for consistency.
- **Confidence:** High (behavior verified via probe); Medium on which policy is intended.

### A12-2: Random `_setstate_` error paths are untested (only the positive round-trip is)
- **Severity:** coverage-gap
- **Location:** `stdlib_random.hpp:288-312` (`_setstate_`)
- **Category:** coverage — deserialization error handling
- **Description:** `random_generators.ki:139-140` asserts the POSITIVE `_getstate_`/`_setstate_`
  round-trip and engine-kind restore, but no test exercises the four rejection paths. Probe confirms
  all four throw correctly today (missing `:` → "malformed engine state (missing kind prefix)";
  unknown kind → "unknown generator 'bogus'"; bad stream → "malformed xoshiro state"; non-String →
  "expected the engine-state String"). A regression that dropped one of these (e.g. accepting a
  malformed stream and leaving a default-seeded engine) would go unnoticed. This matters because
  `_setstate_` is reachable from untrusted `serialize.loads`/`dump.loads` input.
- **Failure scenario:** a corrupt/adversarial checkpoint blob is loaded; without the tests a future
  refactor could silently swallow the malformed state and hand back a non-deterministic RNG.
- **Proposed test:** add to `audit_random.ki`/`spec_random.ki` four `assert throws(...)` cases
  mirroring the probe, plus one that a malformed `_setstate_` leaves the pre-call engine unchanged.
- **Proposed fix:** none (add tests only).
- **Confidence:** High.

### A12-3: C++ `test_random.cpp` covers only 7 of the 11 distribution methods
- **Severity:** coverage-gap
- **Location:** `tools/tests/unit/test_random.cpp`
- **Category:** coverage — C++ vs `.ki` split
- **Description:** The C++ unit test exercises only `random`/`uniform`/`randint`/`choice`/`choices`/
  `shuffle`/`sample`. It has NO C++-level assertion for `gauss`/`normalvariate`/`expovariate`/
  `randrange`/the `seed()` re-seed method/generator selection (`generator="mersenne_twister"`)/or the
  serialize-determinism round-trip. These ARE well-covered in `.ki` scripts (74× `randrange`, 35×
  `gauss`, 29× `expovariate`, 23× `seed`, plus `random_generators.ki` for both engines + round-trip),
  so this is a redundancy/defense-in-depth gap, not a hole in overall coverage. The embed/native API
  path for these methods (`makeMethod` binding, keyword args) is only validated indirectly.
- **Proposed test:** extend `test_random.cpp` with a `gauss`/`expovariate` determinism assertion, a
  `generator="mersenne_twister"` reproducibility assertion, and a `_getstate_`→`_setstate_` mid-stream
  resume assertion at the C++ level.
- **Proposed fix:** none (add tests).
- **Confidence:** High.

### A12-4: `choices` has no `weights`/`cum_weights` — weighted sampling unsupported (by design)
- **Severity:** Low (completeness / documentation)
- **Location:** `stdlib_random.hpp:214-235` (`choices`)
- **Category:** completeness vs Python
- **Description:** Python's `random.choices(population, weights=, cum_weights=, k=)` supports weighted
  sampling; Kirito's `choices` is `(population, k)` only — uniform WITH replacement. CLAUDE.md documents
  exactly `choices(population, k=1)`, so this is intentional, but the task hunt explicitly asked about
  "choices weights validation (negative/zero-sum weights)": there is no weights surface to validate, so
  that class of bug simply cannot occur. Flagging so the absence is a recorded, deliberate decision
  rather than an oversight — a caller migrating from Python who passes `weights=` gets an "unexpected
  keyword" error, not weighted behavior.
- **Proposed test:** none needed; optionally assert `throws(Function(): return r.choices([1,2],
  weights=[1,1]))` to pin that the kwarg is rejected.
- **Proposed fix:** none unless weighted sampling is desired as a future enrichment.
- **Confidence:** High.

### A12-5: math completeness gaps persist (remainder/isqrt/dist/ldexp/frexp/modf; gcd/lcm/hypot fixed-2-arg)
- **Severity:** Low (completeness) — carried from v1.13 A13-7, unchanged in v1.14
- **Location:** whole `stdlib_math.hpp`
- **Category:** completeness vs Python
- **Description:** Still absent (none required by CLAUDE.md): `remainder` (IEEE-754 remainder, ≠
  `fmod`), `isqrt` (integer sqrt), `dist` (n-dim Euclidean), `ldexp`/`frexp`/`modf`/`nextafter`/`ulp`,
  `fsum`, `sumprod`. Also `gcd`/`lcm`/`hypot` remain fixed 2-arg where CPython 3.9+ is variadic
  (`math.gcd(a, b, c)`, n-dim `hypot`). `isclose` is intentionally routed through the value `.compare`
  method. No behavioral bug — a completeness decision for a future enrichment.
- **Proposed fix:** none required; add `remainder`/`isqrt`/`dist` and make `gcd`/`lcm`/`hypot` variadic
  if Python parity is a goal.
- **Confidence:** High.

### A12-6: `uniform(a, b)` with `a > b` relies on libstdc++ tolerance of a std precondition violation
- **Severity:** Low (note) — already regression-tested for "no crash", not for range
- **Location:** `stdlib_random.hpp:140-148` (`uniform`)
- **Category:** portability note
- **Description:** `uniform(5, 2)` passes `a=5, b=2` straight into `std::uniform_real_distribution<double>(5,2)`,
  whose standard precondition is `a <= b`. libstdc++ tolerates it (probe returned 2.56, i.e. a value in
  [2,5]), matching Python's order-agnostic `uniform`. `r7_matrix_random.ki:186` already pins "reversed
  bounds returns a Float (no crash)". Strictly, a different conforming STL could produce a value outside
  [b,a] or misbehave, and the *range* of the result (not just its type) is untested. Very low risk given
  the fixed libstdc++/libc++ targets.
- **Proposed test:** strengthen the existing assertion to `2.0 <= v <= 5.0` for the reversed case.
- **Proposed fix:** optionally normalize with `if (lo > hi) std::swap(lo, hi);` to make order-agnosticism
  explicit rather than implementation-dependent.
- **Confidence:** Medium.

---

## Coverage-gap sub-task (C++ vs .ki)

| Surface | C++ (`test_random.cpp`) | `.ki` scripts | Gap |
|---|---|---|---|
| random/uniform/randint/choice/choices/shuffle/sample | yes | yes | — |
| gauss/normalvariate/expovariate | no | yes (35/13/29 uses) | C++ only (A12-3) |
| randrange | no | yes (74 uses) | C++ only (A12-3) |
| seed() re-seed method | no | yes (23 uses) | C++ only (A12-3) |
| generator="mersenne_twister" selection | no | yes (random_generators.ki) | C++ only (A12-3) |
| serialize/dump round-trip determinism | no | yes (random_generators + serde suite) | C++ only (A12-3) |
| `_setstate_` error paths | no | no | **untested anywhere (A12-2)** |
| math domain throws | yes (test_domain_guards, test_audit_v113) | yes (spec/audit/verify_math) | — |
| fmod(inf) throw (was A13-1) | yes (test_audit_v113) | yes (verify_math) | now covered |
| gauss/expo/uniform NaN param | no | no | **untested (A12-1)** |

## Summary

- **Critical:** 0
- **High:** 0
- **Medium:** 0 (v1.13's only Medium, A13-1 fmod(inf), is FIXED + tested)
- **Low / note:** 4 (A12-1 NaN-param bypass, A12-4 no weighted choices, A12-5 completeness, A12-6 uniform a>b)
- **Coverage-gap:** 2 (A12-2 `_setstate_` error paths, A12-3 thin C++ random tests)

Overall: math + random is one of the most robust subsystems audited. No actionable bug of Medium+
severity. Strongest new observation: A12-1 (NaN slips past distribution param validation → silent NaN),
worth a policy decision. Everything else is coverage/completeness.
