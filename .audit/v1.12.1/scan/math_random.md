# v1.12.1 (audit loop) — math and random

Source: `src/kirito/stdlib_math.hpp` (285 lines), `src/kirito/stdlib_random.hpp` (357 lines).
Probe binary: `./build-debug/ki`.

## LOG
- Read both files fully.
- Math: unary lambda has optional `ok` domain predicate; NaN passes through, domain-fail throws
  "math domain error". fmod/log/pow/atan2/hypot/gcd/lcm/factorial/prod/comb/perm bespoke.
- Random: RandomState variant<xoshiro,mt19937_64>; distributions via std::visit.
- Hypotheses to probe:
  - H1: `uniform(a, b)` checks finite but NOT `a <= b`; `std::uniform_real_distribution(a,b)` is UB when a>b.
  - H2: verify every documented math domain input throws.
  - H3: serialize/dump round-trip restores exact stream.

## VERIFIED CORRECT (ruled out)
- **math domain errors**: ALL documented boundaries throw "math domain error" correctly —
  sqrt(-1), asin(2)/acos(±>1), acosh(<1), atanh(±1), log(0)/log(-1), log(x,base<=0 or ==1),
  log2(0), log10(0), log1p(<=-1), gamma/lgamma at non-positive integers, pow(neg,frac),
  pow(0,neg), fmod(x,0), fmod(inf,y). NaN passes through (sqrt/asin(nan)->nan). Overflow to inf
  (exp(1000), gamma(200)) is a range not domain condition, passes through — correct.
- **gamma(-0.5)** correctly returns a value (only integer poles throw). Correct.
- **fmod(2, inf)=2.0** (finite dividend, inf divisor) does NOT throw — correct per libm.
- **gcd/lcm**: gcd(0,0)=0, gcd(-12,8)=4, lcm(0,5)=0, lcm(-4,6)=12; INT64_MIN magnitude via
  unsigned (no std::abs UB); lcm overflow via __builtin_mul_overflow + INT64_MAX check. Solid.
- **factorial/comb/perm/prod**: negatives rejected, overflow throws (factorial(21), prod huge,
  comb(5,-1)); comb uses 128-bit intermediate widening; perm(n)=n!. floor/ceil use
  toInt64Checked (NaN/inf/out-of-range all throw; 2^63 boundary correct). Solid.
- **constants**: pi/e/tau/inf/nan exact. degrees/radians/hypot/atan2/copysign correct.
- **random validation**: randint(a>b) throws, gauss sigma<0 / non-finite throws, expovariate
  <=0/non-finite throws, randrange step==0 / empty / too-large-span throws, choice/choices/sample
  on empty or non-iterable throws, choices k<0/too-large throws, sample k out of range throws.
  Random(float seed) throws, Random(bad generator) throws. Solid.
- **serialize/dump exact-stream round-trip CONFIRMED** for BOTH xoshiro and mersenne_twister:
  seed->draw->snapshot->draw-more vs restore->draw produce byte-identical future draws
  (`future == restored` True), and generator kind is preserved. Cross-engine restore (apply an
  xoshiro state to a Random built as mersenne_twister) correctly switches the engine and
  reproduces the stream.
- **_setstate_ on corrupt blob**: empty / no-colon / unknown-kind / garbage-engine-state /
  non-String all throw clean catchable errors. Robust.
- **seed()** method re-seeds current engine (does not switch kind), reproducible, accepts negative.
- **shuffle** is correct Fisher-Yates; **sample** correct selection sampling; both memory-safe
  (RootScope pins the pool across allocations).

## FINDINGS

### F1 [LOW/SUSPECT] random.uniform(a, b) with a > b relies on UB in std::uniform_real_distribution
- where: src/kirito/stdlib_random.hpp:141-151 (uniform)
- repro: `random.Random(42).uniform(5, 1)` => 1.74.., 3.72.. (values land in [1,5])
- actual: produces sensible values inside [b, a] (matches Python's `a+(b-a)*random()` semantics,
  which explicitly supports a>b). But it passes lo>hi straight into
  `std::uniform_real_distribution<double>(lo, hi)`, whose standard precondition is `a <= b`;
  a>b is UNDEFINED BEHAVIOR per [rand.dist.uni.real]. libstdc++ happens to compute
  `g*(b-a)+a` so it works today; another conforming stdlib (or a hardened/debug build) could
  assert, return NaN, or loop.
  expected: either normalize (swap so lo<=hi) like Python does explicitly, or reject a>b — do not
  hand an out-of-precondition range to the distribution. Note: uniform already validates
  finiteness but not ordering. (randint DOES reject lo>hi, so the two are inconsistent.)
- fix idea: `if (lo > hi) std::swap(lo, hi);` before constructing the distribution (Python-compatible),
  or throw "uniform: a must be <= b" for symmetry with randint.
- NOTE: this is the ONLY defect found; produces correct-looking output in the current build, so
  severity is LOW. Everything else in math/random is well-hardened.
