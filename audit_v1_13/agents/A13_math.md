# A13 — `math` module audit (`src/kirito/stdlib_math.hpp`)

- **Agent:** A13
- **Area:** math standard library module
- **Scope (read-only):** `src/kirito/stdlib_math.hpp` (279 lines) — every constant + function.
- **Existing tests reviewed:** `tools/tests/scripts/probe_math_vectors.ki`, `spec_math.ki`,
  `audit_math.ki`, `spec_domain_guards.ki`, `r9_math_extra.ki`, `r5_math_sys.ki`, `r6_math.ki`,
  `r7_math.ki`, `r8_math.ki`, `deep_numeric.ki`, `tools/tests/unit/test_numeric_deep.cpp`,
  `test_domain_guards.cpp`, `test_audit_hardening.cpp`, `test_hardening2.cpp`, `test_adversarial.cpp`,
  and `tools/tests/errors/math_domain_*.{ki,experr}` + `factorial_negative.ki` + `gcd_non_integer.ki`.
- **Contract (CLAUDE.md):** domain errors THROW a clear `math domain error`; NaN passes through
  unchanged; overflow-to-`inf` is a RANGE (not domain) condition and does NOT throw.
- **Method:** static reasoning; no build/run performed.

## Surface enumerated

**Constants (5):** `pi`, `e`, `tau` (=2π), `inf` (=`HUGE_VAL`), `nan` (=`std::nan("")`).

**Unary functions via the shared `unary` helper (24):** `sqrt`[x≥0], `cbrt`, `sin`, `cos`, `tan`,
`asin`[−1..1], `acos`[−1..1], `atan`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh`[x≥1], `atanh`[(−1,1)],
`exp`, `expm1`, `log1p`[x>−1], `log2`[x>0], `log10`[x>0], `trunc`, `gamma`[not a non-positive-integer
pole], `lgamma`[same], `erf`, `erfc`. (Guards in `[...]`; the rest are unguarded, correctly.)

**Other functions (20):** `isnan`, `isinf`, `isfinite`, `copysign(x,y)`, `fmod(x,y)`[y≠0],
`lcm(a,b)`, `log(x[,base])`[x>0; base>0 and ≠1], `pow(x,y)`[x<0⇒int y; x=0⇒y≥0], `atan2(y,x)`,
`hypot(x,y)`, `fabs`, `degrees`, `radians`, `floor→Int`, `ceil→Int`, `factorial(n)`[n≥0, overflow],
`gcd(a,b)`, `prod(iterable[,start])`[int overflow], `comb(n,k)`, `perm(n[,k])` [both non-negative,
overflow].

**Overall verdict:** the module is unusually well-hardened. Overflow paths use unsigned-magnitude /
`__builtin_mul_overflow` / `__int128`, `INT64_MIN` is handled without `std::abs` UB, `toInt64Checked`
guards NaN/inf/range before every double→int cast, and NaN-passthrough/overflow-to-inf are honored.
No Critical/High confirmed bug found. Findings are one Medium policy-divergence (`fmod(inf,·)` silent
NaN), a set of coverage gaps (mostly at `INT64_MIN` boundaries and unguarded-function NaN/inf), and a
DRY note about the domain-guard policy being triplicated across math/complex/tensor.

---

## Findings

### A13-1: `fmod(inf, finite)` returns a silent NaN instead of throwing a domain error
- **Severity:** Medium
- **Location:** `stdlib_math.hpp:101-105` (`fmod`)
- **Category:** wrong/absent domain guard (silent-NaN policy divergence)
- **Description:** The `fmod` guard only rejects a zero divisor (`y == 0.0`). When the *dividend* is
  infinite and the divisor finite non-zero (`fmod(inf, 3)`, `fmod(-inf, 3)`), `std::fmod` returns
  `NaN`. The input is not NaN, so this is exactly the "silent NaN/inf rubbish" the CLAUDE.md contract
  says must THROW. CPython agrees: `math.fmod(inf, 1)` raises `ValueError: math domain error`.
  Kirito instead hands back a quiet `NaN`.
- **Failure scenario:** `import("math").fmod(math.inf, 3)` → `nan` (no error), so a program that
  relies on the throw-don't-return-NaN invariant silently propagates NaN. `fmod(x, nan)` and
  `fmod(nan, x)` are correct passthroughs (input NaN), but `fmod(inf, finite)` is not.
- **Proposed test:** add to `spec_math.ki`/`r7_math.ki`: `assert throws(Function(): return
  math.fmod(math.inf, 3.0))` and `assert throws(Function(): return math.fmod(0.0-math.inf, 3.0))`.
- **Proposed fix:** in `fmod`, after the zero-divisor check add
  `if (std::isinf(x) && !std::isnan(y)) throw KiritoError("fmod: math domain error (dividend is infinite)");`
  (mirroring CPython, which throws when the first arg is infinite / second is zero).
- **Confidence:** High (behavior), Medium (whether the project wants to match CPython vs. treat it as
  benign range NaN — but the stated policy is "throw, don't return NaN rubbish").

### A13-2: `INT64_MIN` boundary of `gcd`/`lcm`/`comb`/`perm`/`factorial` is untested
- **Severity:** coverage-gap
- **Location:** `gcd` (182-196), `lcm` (106-124), `factorial` (171-181), `comb`/`perm` (228-270)
- **Category:** coverage — adversarial integer extremes
- **Description:** The magnitude helpers `mag(v)` at lines 113-115 / 188-190 exist specifically to
  avoid `std::abs(INT64_MIN)` UB, and the overflow guards (`> INT64_MAX`) exist to reject results
  that don't fit. But no test passes `INT64_MIN` (`-9223372036854775808`) directly to any of these.
  Existing tests only exercise small negatives (`gcd(-12,18)`, `lcm(-4,6)`) and the fitting/overflow
  boundary via `comb(62,31)`/`comb(64,32)`/`perm(20)`/`perm(21)`/`factorial(20)`/`factorial(21)`.
- **Failure scenario (verified by reasoning, expected to PASS — this is a missing test, not a bug):**
  `gcd(IMIN, IMIN)` → magnitude `2^63` > `INT64_MAX` → should throw "gcd result too large";
  `gcd(IMIN, 0)` → `2^63` → should throw; `lcm(IMIN, 1)` → `2^63` → should throw;
  `math.factorial(IMIN)` → n<0 → should throw "not defined for negatives";
  `comb(IMIN, 2)`/`perm(IMIN, 2)` → non-negative check throws.
- **Proposed test:** in `test_numeric_deep.cpp` add `var IMIN = -9223372036854775808` and assert each
  of the above throws (and that no ASan/UBSan report fires — the whole point of `mag`).
- **Proposed fix:** none (add tests only).
- **Confidence:** High.

### A13-3: `log2`/`log10`/`log1p` tested only at the exact boundary, not deeper negatives
- **Severity:** coverage-gap
- **Location:** `stdlib_math.hpp:80-82`
- **Description:** `log2(0)`/`log10(0)`/`log1p(-1)` throw-tests exist, but negative arguments strictly
  inside the forbidden region (`log2(-5)`, `log10(-3)`, `log1p(-2)`) are not tested. The guards
  (`x>0`, `x>0`, `x>-1`) cover them, but a regression that flipped a comparator would only be caught
  at the boundary today.
- **Proposed test:** `assert throws(...log2(-5))`, `...log10(-3)`, `...log1p(-2)`.
- **Proposed fix:** none.
- **Confidence:** High.

### A13-4: Unguarded functions' NaN/inf passthrough is only partially pinned
- **Severity:** coverage-gap
- **Location:** `cbrt`, `atan`, `asinh`, `expm1`, `erf`, `erfc`, `trunc`, `degrees`, `radians`,
  `fabs`, `atan2`, `hypot`, `copysign` (various lines)
- **Description:** `r8_math.ki` pins NaN passthrough for most unary functions, and `r7_math.ki` pins
  copysign/atan2/hypot/fmod/pow/log NaN behavior. Gaps: `degrees(nan)`/`radians(nan)`/`fabs(nan)`/
  `cbrt(inf)`/`atan(inf)`(=π/2)/`asinh(inf)`(=inf)/`erf(inf)`(=1)/`erfc(-inf)`(=2)/`trunc(inf)`
  behavior is not asserted anywhere. All are correct by libm passthrough; missing assertions only.
- **Proposed test:** a small block asserting `isnan(degrees(nan))`, `isnan(radians(nan))`,
  `isnan(fabs(nan))`, `erf(math.inf)==1.0`, `erfc(0.0-math.inf)==2.0`, `atan(math.inf)≈pi/2`.
- **Proposed fix:** none.
- **Confidence:** High.

### A13-5: `pow(negative, 1/3)`-style real roots throw — documented gotcha, no `cbrt` cross-note in tests
- **Severity:** Low
- **Location:** `stdlib_math.hpp:137-147` (`pow`)
- **Category:** behavior gotcha (matches CPython, not a bug)
- **Description:** `pow(-8, 1.0/3.0)` throws "a negative base requires an integer exponent" even
  though a real cube root (−2) exists. This matches `math.pow` in CPython (which also raises), and the
  intended path is `cbrt`. Worth an explicit regression asserting BOTH that `pow(-8, 1.0/3.0)` throws
  AND that `cbrt(-8.0) == -2.0` (the latter exists in `spec_math.ki`; the pair is not co-located to
  document the intent).
- **Proposed test:** co-locate `assert throws(...pow(-8.0, 1.0/3.0))` next to `cbrt(-8.0)==-2.0`.
- **Proposed fix:** none (behavior is intentional).
- **Confidence:** High.

### A13-6: `prod` rejects `Bool` elements/start — divergence from Python's int-like bools
- **Severity:** Low
- **Location:** `stdlib_math.hpp:198-225` (`prod`)
- **Category:** behavior divergence
- **Description:** `prod([True, False])` throws "prod expects numbers" and `prod([2], True)` throws
  "prod start must be a number", because the element/start type checks accept only `Integer`/`Float`,
  not `Bool`. `Value::asFloat` similarly rejects Bool, so every math function refuses a Bool argument.
  This is internally consistent (Bool is not a number in Kirito) but diverges from Python where
  `math.prod([True, True, 2])==2`. Likely intentional; flagging for the record.
- **Failure scenario:** `math.prod([True, True])` → throws instead of `1`.
- **Proposed test:** if intentional, pin it: `assert throws(Function(): return math.prod([True]))`.
- **Proposed fix:** none unless Bool-as-number is desired project-wide.
- **Confidence:** High.

### A13-7: Missing common `math` functions vs. Python (completeness)
- **Severity:** Low (completeness-gap)
- **Location:** whole module
- **Description:** Not present (out of the documented set, but commonly expected): `remainder`
  (IEEE 754 remainder, differs from `fmod`), `ldexp`, `frexp`, `modf`, `nextafter`, `ulp`, `dist`
  (n-dim Euclidean distance), `fsum` (exact summation), `isqrt` (integer sqrt), `isclose`
  (Kirito routes this through the value `.compare` method instead). `hypot`/`atan2` are fixed 2-arg
  (Python's `hypot` is variadic). None are required by CLAUDE.md; noting for a future enrichment
  decision. The `.compare` method already covers `isclose`, so that one is by-design.
- **Proposed fix:** none required; optionally add `remainder`/`dist`/`isqrt` if desired.
- **Confidence:** High.

### A13-8: Domain-guard POLICY is triplicated across `math`, `complex`, `tensor` (DRY / divergence risk)
- **Severity:** dry
- **Location:** `stdlib_math.hpp` (this module), `stdlib_complex.hpp`, `stdlib_tensor.hpp`
- **Category:** DRY / cross-module consistency
- **Description:** The "out-of-domain input THROWS a `math domain error`; NaN passes; overflow→inf is
  range" policy is implemented independently in three modules (CLAUDE.md acknowledges this by design).
  The three do not share a predicate table, so a change to one (e.g. tightening `atanh` to reject
  exactly ±1, or A13-1's `fmod(inf,·)` fix) can silently drift from the others. `test_domain_guards.cpp`
  is the only place that checks all three together, and it does not cover `fmod(inf,·)`, `pow` overflow,
  or the `INT64_MIN` combinatoric boundaries. Concrete divergence already exists: `math.pow(-8,1/3)`
  throws, but `complex.pow` and `tensor.pow` on the same real inputs behave differently (complex has a
  valid answer; tensor throws) — correct per type, but the *shared* guard helper does not exist to make
  that intentional-vs-accidental explicit.
- **Proposed fix:** none structural required; if consolidated later, a shared
  `mathDomain::guardX(...)` free-function set would make the policy a single source of truth. At
  minimum, extend `test_domain_guards.cpp` to lock the cross-module edges named above.
- **Confidence:** Medium (this is a maintainability observation, not a defect).

### A13-9: `unary` helper's redundant `a.size() != 1` guard (dead but harmless)
- **Severity:** Low (code-quality note, not a bug)
- **Location:** `stdlib_math.hpp:57` (and similar explicit `a.size()!=2` guards in `pow`/`atan2`/
  `hypot` at 138/149/153)
- **Description:** Signatured natives are dispatched through `NativeFunction::bindArgs`, which already
  guarantees the exact positional arity before the impl runs, so the in-lambda `if (a.size() != 1)
  throw ...` (and the 2-arg equivalents) can never fire. Harmless defensiveness; not all sibling
  functions have it (e.g. `copysign`/`fmod`/`isnan` omit it), so it is also inconsistent. No action
  needed; noted for completeness of the audit.
- **Proposed fix:** optionally drop the dead checks for uniformity, or keep as belt-and-braces.
- **Confidence:** High.

### A13-10: `toInt64Checked` rejects `floor`/`ceil` of values within ~1 ULP of `INT64_MAX`
- **Severity:** Low (edge behavior, likely intentional)
- **Location:** `stdlib_math.hpp:29-35`, used by `floor` (165) / `ceil` (168)
- **Description:** The upper guard is `d >= 9223372036854775808.0` (2^63). Because doubles near
  `INT64_MAX` round up to exactly `2^63`, `floor(9223372036854775807.0)` and any input in the top
  ~1024-integer band throw "result out of Integer range" even though a representable int64 arguably
  exists. This is a safe/conservative choice (the double literally cannot represent `INT64_MAX`), and
  `audit_math.ki` already asserts `floor(1e30)` throws, so the throw-on-huge behavior is pinned — but
  the exact `2^63−ε` boundary is untested and the conservatism is undocumented.
- **Failure scenario:** `math.floor(9.223372036854775e18)` throws rather than returning a large int64.
- **Proposed test:** pin the intended boundary once decided (e.g. assert `floor(9.0e18)` succeeds and
  `floor(9.3e18)` throws).
- **Proposed fix:** none (the conservative guard is correct for avoiding UB); optionally document.
- **Confidence:** Medium.

---

## Coverage matrix (function × domain condition → tested?)

| Function | normal | boundary | domain-throw | NaN passthrough | inf/overflow |
|---|---|---|---|---|---|
| sqrt | ✅ | ✅(0) | ✅(-1) | ✅ | ✅(1e100) |
| cbrt | ✅ | ✅(-8) | n/a | ✅(r8) | ⬜ inf |
| sin/cos/tan | ✅ | ✅(π) | n/a | ✅ | n/a |
| asin/acos | ✅ | ✅(±1) | ✅(2,-2) | ✅ | n/a |
| atan | ✅ | ✅ | n/a | ✅ | ⬜ atan(inf) |
| sinh/cosh/tanh | ✅ | ✅ | n/a | ✅ | ✅(1000) |
| asinh | ✅ | — | n/a | ✅ | ⬜ |
| acosh | ✅ | ✅(1) | ✅(0,0.5) | ✅ | ⬜ |
| atanh | ✅ | ✅ | ✅(±1,2) | ✅ | n/a |
| exp/expm1 | ✅ | ✅(0) | n/a | ✅ | ✅(1000) |
| log | ✅ | ✅(1) | ✅(0,-1,base1,base-2) | ✅ | ✅(log(inf)) |
| log2/log10 | ✅ | ✅(0) | ✅(0) | ✅ | — deep-negative ⬜ |
| log1p | ✅ | ✅(0,-1) | ✅(-1) | ✅ | ⬜(-2) |
| gamma/lgamma | ✅ | ✅(0,-1,-2,-5) | ✅ | ✅(lgamma) | ✅(gamma(200)) |
| erf/erfc | ✅ | ✅(0) | n/a | ✅(erf) | ⬜ erf(inf)/erfc(-inf) |
| floor/ceil | ✅ | ✅(neg) | ✅(nan,inf,1e30) | via throw | ✅ |
| trunc | ✅ | ✅(neg) | n/a | ⬜ | ⬜ trunc(inf) |
| copysign | ✅ | ✅ | n/a | ✅(r7) | n/a |
| fmod | ✅ | ✅(signs) | ✅(y=0) | ✅(nan args) | **⬜ fmod(inf,·) — A13-1** |
| atan2/hypot | ✅ | ✅(0,0) | n/a | ✅(r7) | ✅(hypot inf) |
| fabs/degrees/radians | ✅ | ✅ | n/a | ⬜ | ⬜ |
| pow | ✅ | ✅ | ✅(-2^.5, 0^-1, base1) | ✅(r7) | ✅(10^400) |
| gcd/lcm | ✅ | ✅(0,neg) | overflow ✅ | n/a | ✅ / **IMIN ⬜ A13-2** |
| factorial | ✅ | ✅(0,20) | ✅(-1,21,1.5) | n/a | ✅ / **IMIN ⬜ A13-2** |
| comb/perm | ✅ | ✅(62,64,20,21) | ✅(negatives) | n/a | ✅ / **IMIN ⬜ A13-2** |
| prod | ✅ | ✅([],start) | overflow ✅ | n/a | ✅(inf floats) / Bool ⬜(A13-6) |
| isnan/isinf/isfinite | ✅ | ✅ | n/a | n/a | ✅ |
| constants pi/e/tau/inf/nan | ✅ | — | — | — | — |

✅ = tested, ⬜ = gap, — = not applicable.
