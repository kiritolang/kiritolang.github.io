# A11 — Builtins Audit (Kirito v1.13)

Area: builtins (`range`, `sum`, `min`, `max`, `abs`, `round`, `sorted`, `enumerate`, `zip`, `map`,
`filter`, `len`, `type`, `id`, `import`, `inspect`, `all`, `any`, `reversed`, `divmod`, `isinstance`,
`hasattr`, `ord`, `chr`, `bin`, `oct`, `hex`, `pow`, `bitand`/`bitor`/`bitxor`/`bitnot`, `shl`/`shr`,
`format`, and the `Integer`/`Float`/`String`/`Bool`/`List`/`Set`/`Dict`/`Bytes` constructors).

Method: static read-only analysis. No builds run. Findings below; confirmed bugs first, then
weak-spots, then coverage gaps, then DRY.

Note: `builtins.hpp` itself holds only the value classes (`NoneVal`/`BoolVal`/`StrVal`/`IntVal`/
`FloatVal`) and UTF-8/float-format helpers. The builtin *functions* are registered in another file —
locating & reading it below.

---

## Scope confirmed

Builtin *functions* are registered in `KiritoVM::installBuiltins()` at `src/kirito/runtime.hpp:2780-3418`.
`builtins.hpp` holds only the value classes + UTF-8/float helpers. Shared impls used by builtins:
`numericBinary`/`wadd`/`wsub`/`wmul`/`ifloordiv`/`imod`/`ipow` (runtime.hpp:98-247), `applyFormatSpec`
(runtime.hpp:2620), `resolveTypeName`/`typeMatches` (runtime.hpp:1929-1967), `rootedIterate`
(runtime.hpp:2773), `makeBytes` (bytes.hpp:312), `kMaxRepeat` (common.hpp:116).

## Overall assessment

This area is **exhaustively tested**. `tools/tests/scripts/audit_builtins.ki` (849 lines) probes every
builtin with typical/edge/error/fuzz angles, and the `r4_`–`r11_` regression families plus
`test_builtins_deep.cpp`, `test_audit_hardening.cpp`, `test_audit_v112.cpp`, `test_hardening2.cpp`,
`test_domain_guards.cpp`, and the `tools/tests/errors/*.experr` message suite cover the adversarial
and resource-limit corners. Nearly every hypothesized gap turned out already tested:
`abs(INT64_MIN)` wrap, `chr(0xD800)` surrogate, `Float("inf"/"nan")`, `round(2.675,2)` double-rounding,
`round` extreme ndigits (`±400`/`±1000`), `pow` 3-arg large-modulus (`__int128` path) and negative
modulus, `enumerate(start=INT64_MAX)` wrap, `Integer("0xFFFFFFFFFFFFFFFF")==-1`, `Integer("0x0x5")`
rejection, `min/max default` ignored when non-empty, `abs(True)`/`round(True)` rejection,
`shl(1,63)`/`shl(1,64)`, `range` too-large and INT64 straddle. No confirmed correctness bugs found.

Findings below are therefore limited: one cross-platform weak-spot, one DRY duplication, and a few
genuinely-untested residual corners.

---

### A11-1: round(x, ndigits) precision depends on 80-bit `long double`; wrong where `long double == double` (MSVC)
- severity: Medium
- location: `src/kirito/runtime.hpp:2997-3001` (round ndigits branch)
- category: weak-spot (cross-platform)
- description: The anti-double-rounding fix scales with `long double`
  (`std::pow(10.0L, nd)`, `std::round((long double)x * f) / f`). The comment and the golden test
  `r4_numbers.ki:239` / `r6_builtins.ki:284` assert `round(2.675, 2) == 2.67`, which only holds when
  `long double` has more precision than `double`. On targets where `long double == double`
  (MSVC/native Windows — a documented minimum-support platform per CLAUDE.md "static CRT on MSVC"),
  the intermediate `2.675*100` rounds to `267.5` → `268` → `round(2.675,2) == 2.68`, contradicting the
  test and the documented half-away-from-zero-without-double-rounding behavior.
- failure-scenario: Native MSVC build: `round(2.675, 2)` returns `2.68`; the golden `.ki` suite
  assertion fails. (Latent because CI builds/tests only on Linux and cross-compiles Windows via
  mingw-w64, whose `long double` is 80-bit, so the discrepancy never surfaces in CI.)
- proposed-test: A CI job (or documented note) exercising `round(2.675, 2)` on a `long double ==
  double` toolchain; or a `static_assert`/`#if` around this code path selecting a double-double or
  string-based scaling when `LDBL_MANT_DIG == DBL_MANT_DIG`.
- proposed-fix: Guard with `#if LDBL_MANT_DIG > DBL_MANT_DIG` and fall back to a snprintf/round-to-
  string strategy (or Grisu-style) when extended precision is unavailable, so rounding is identical
  on every platform.
- confidence: Medium (exact FP result on MSVC not run here, but the code explicitly relies on extended
  precision that MSVC does not provide).

### A11-2: Integer→base-digits loop duplicated between `radix` (bin/oct/hex) and `applyFormatSpec`
- severity: dry
- location: `src/kirito/runtime.hpp:3345-3350` (`radix` lambda) vs `src/kirito/runtime.hpp:2707-2714`
  (applyFormatSpec integer branch)
- category: dry
- description: The identical "negate-to-unsigned, emit digits via `alpha[u % base]`, reverse" loop
  appears twice (plus a third near-identical copy in `Integer(String)`'s inverse direction). Both
  radix and the format integer path compute the same magnitude/sign/digit sequence with the same
  `"0123456789abcdef"` alphabet.
- failure-scenario: A future change to base-formatting (e.g. a new base, or a sign-handling fix) must
  be applied in two places or the two builtins drift (`hex(n)` vs `format(n,"x")` producing different
  output for an edge like INT64_MIN).
- proposed-test: A property test asserting `hex(n)[stripprefix] == format(n,"x")` (and o/b) over random
  n including INT64_MIN.
- proposed-fix: Extract a shared `intToBaseDigits(uint64_t u, int base, bool upper) -> std::string`
  helper and call it from both `radix` and `applyFormatSpec`.
- confidence: High.

### A11-3: Native parameter type annotations are documentary only — NOT enforced by `bindArgs`
- severity: Low
- location: `src/kirito/function.hpp:78-102` (`NativeFunction::bindArgs`)
- category: weak-spot
- description: `bindArgs` binds positionals/keywords and fills defaults but never checks the
  `NativeParam::annotation` field. So `abs`'s `{"x","Number"}`, `bin`'s `{"n","Integer"}`, `ord`'s
  `{"char","String"}`, etc. are shown by `inspect` but do not gate the call — every current impl
  re-validates the type itself (abs: "abs expects a number"; bint: "must be an Integer"; etc.). This
  differs from Kirito-defined functions, whose annotations ARE runtime-enforced (`typeMatches` in
  `KiFunction::callFull`, runtime.hpp:2054/2063). The asymmetry is a latent trap: a newly-added
  signatured native that trusts its annotation and omits its own guard would silently accept wrong
  types (and could then downcast — type-confusion).
- failure-scenario: A contributor adds `defSig("twice", {{"x","Integer"}}, "Integer", ...)` and does
  `static_cast<const IntVal&>(o).value()` without a kind check, assuming the "Integer" annotation was
  enforced; `twice(3.5)` reinterprets a FloatVal as IntVal (UB) instead of throwing.
- proposed-test: A unit test asserting a signatured native with a mismatched positional does NOT throw
  from the binder (documenting current behavior), plus a doc/comment that native impls must self-check.
  Better: make `bindArgs` optionally enforce annotations via `typeMatches`.
- proposed-fix: Either enforce annotations in `bindArgs` (calling `typeMatches`, matching Kirito-fn
  semantics) or add a prominent comment on `NativeParam::annotation` that it is advisory-only for
  natives; audit all impls confirm self-validation.
- confidence: High (behavior confirmed from `bindArgs` source); severity Low because all current impls
  do self-validate.

### A11-4: `divmod(INT64_MIN, -1)` and `divmod` with non-numeric operands untested
- severity: coverage-gap
- location: impl `src/kirito/runtime.hpp:3277-3286`; helpers `ifloordiv`/`imod` (runtime.hpp:111-122)
- category: coverage-gap
- description: `divmod` delegates to `numericBinary(FloorDiv)` + `numericBinary(Mod)`. `divmod(5,0)`
  (zero divisor) is tested (audit_builtins.ki:534), but the two other adversarial corners are not:
  (a) `divmod(INT64_MIN, -1)` — the overflow case that `ifloordiv`/`imod` special-case (`b==-1`),
  expected `[INT64_MIN, 0]`; (b) `divmod("a", 2)` / `divmod(1, [])` — non-numeric operand, expected a
  clean "unsupported operand type" from `numericBinary`'s guard (runtime.hpp:161).
- failure-scenario: A regression in the `b==-1` special-case (removing it) would reintroduce
  INT64_MIN/-1 signed-overflow UB, uncaught by the suite.
- proposed-test: `assert divmod(-9223372036854775808, -1) == [-9223372036854775808, 0]` and
  `assert throws(Function(): return divmod("a", 2))`.
- proposed-fix: none (add tests).
- confidence: High (grep of the test tree found neither case).

### A11-5: `range` lone-positional reinterpreted as `start` when `stop`/`end` is a keyword — untested edge
- severity: coverage-gap
- location: `src/kirito/runtime.hpp:3036-3045`
- category: coverage-gap
- description: When a single positional is given AND `stop` (or its `end` alias) was supplied as a
  keyword, the `a.size()==1 && !hasStop` overload does NOT fire, so the lone positional binds to
  `start` instead of `stop` (e.g. `range(5, stop=3)` → `range(start=5, stop=3)` → `[]`;
  `range(5, end=8)` → `[5,6,7]`). This is defensible per the "positionals first, keywords fill" rule
  but is a surprising, unexercised path. The suite tests keyword-only and the positional/keyword
  *clash* (`range(2,5,start=1)`) but not this reinterpretation.
- failure-scenario: Silent behavior change here (e.g. making a lone positional always mean stop) would
  be undetected.
- proposed-test: `assert range(5, end=8) == [5,6,7]` and `assert range(5, stop=3) == []`.
- proposed-fix: none (add tests); optionally document the precedence in docs/pages/08-builtins.md.
- confidence: Medium.

### A11-6: `sum` INT64 wraparound and mixed-position Float promotion under overflow untested
- severity: coverage-gap
- location: `src/kirito/runtime.hpp:3081-3100`
- category: coverage-gap
- description: `sum` accumulates integers with `wadd` (two's-complement wrap) and tracks a parallel
  `double`. `enumerate`'s INT64_MAX wrap is pinned (r8_builtins.ki:224) but `sum`'s is not: e.g.
  `sum([9223372036854775807, 1])` should wrap to `INT64_MIN`. Also untested: a Float appearing
  *after* many large ints (the `n`/`f` dual-accumulator switching to Float mid-iteration) —
  correctness of the returned `f` when the running int part has wrapped.
- failure-scenario: A change to `sum`'s accumulator that broke the wrap or the Float switchover would
  pass the current suite.
- proposed-test: `assert sum([9223372036854775807, 1]) == -9223372036854775808` and a mixed case
  `sum([9223372036854775807, 9223372036854775807, 2.0])` checking the Float result.
- proposed-fix: none (add tests).
- confidence: Medium.

### A11-7: `List`/`Set`/`Dict` constructors from a Dict/Set source (non-List iterable) untested
- severity: coverage-gap
- location: `src/kirito/runtime.hpp:2937-2968`
- category: coverage-gap
- description: The constructors accept "any iterable" via `rootedIterate`. Tested sources: List, String,
  None. Untested: `List({"a":1,"b":2})` (→ keys), `Set({"a":1})` (→ keys), `List({1,2,3})` (from a
  Set — arbitrary order), and `Dict` from a Set/String of pairs. Iterating a Dict/Set mid-build also
  exercises the GC-rooting path (`rootedIterate`) for those kinds.
- failure-scenario: A change to Dict/Set iteration order or protocol used by the constructors would be
  undetected for the constructor path specifically.
- proposed-test: `assert sorted(List({"a":1,"b":2})) == ["a","b"]`; `assert Set({"x":1}) == {"x"}`.
- proposed-fix: none (add tests).
- confidence: Medium.

### A11-8: `hasattr` catches only `KiritoError`; a native `getAttr` throwing a plain `std::exception` would escape
- severity: Low
- location: `src/kirito/runtime.hpp:3310-3315`
- category: weak-spot
- description: `hasattr` probes `getAttr` inside `try { ... } catch (const KiritoError&)`. The comment
  asserts `getAttr` "runs no user code and has no side effects for any Kirito type." That holds for the
  in-tree types, but a third-party `NativeClass::getAttr` (the extension surface Kirito advertises) that
  throws a `std::runtime_error`/`std::bad_alloc` rather than a `KiritoError` would not be caught by
  `hasattr` — it would propagate as an uncaught C++ exception past the intended boolean result. (A bare
  Kirito `catch` elsewhere does absorb `std::exception`; `hasattr`'s internal probe does not.)
- failure-scenario: `hasattr(thirdPartyObj, "x")` where the object's `getAttr` throws `std::out_of_range`
  → escapes as an uncaught exception instead of returning `False`.
- proposed-test: A `NativeClass` test double whose `getAttr` throws `std::runtime_error`, asserting
  `hasattr(obj, "missing") == False` (not a crash).
- proposed-fix: Broaden the probe to `catch (const std::exception&)` (or `catch (...)`), consistent with
  Kirito's boundary policy that a native throw becomes a clean result.
- confidence: Medium (theoretical — no in-tree native currently does this).

---

## Summary

Counts:
- Medium: 1 (A11-1 round long-double portability)
- Low: 2 (A11-3 native annotations not enforced, A11-8 hasattr catch scope)
- dry: 1 (A11-2 base-digit loop duplication)
- coverage-gap: 4 (A11-4 divmod edges, A11-5 range positional-vs-keyword, A11-6 sum wrap, A11-7 constructors from Dict/Set)
- Critical/High: 0 (no confirmed correctness bugs)

Top 5:
1. A11-1 (Medium) — `round(x, ndigits)` relies on 80-bit `long double`; produces a different result
   (and fails the golden `round(2.675,2)==2.67` test) on MSVC where `long double == double`. Latent
   because CI is Linux + mingw only.
2. A11-3 (Low) — native param annotations are documentary; `bindArgs` never enforces them (unlike
   Kirito-fn annotations). Safe today only because every impl self-validates; a latent type-confusion
   trap for future natives.
3. A11-2 (dry) — the integer→base-digit conversion loop is duplicated in `radix` (bin/oct/hex) and
   `applyFormatSpec`; extract a shared helper.
4. A11-8 (Low) — `hasattr` catches only `KiritoError`; a third-party `NativeClass::getAttr` throwing a
   plain `std::exception` would escape instead of yielding `False`.
5. A11-4/5/6/7 (coverage) — residual untested corners: `divmod(INT64_MIN,-1)` & non-numeric operands,
   `range` lone-positional-with-keyword-stop, `sum` INT64 wraparound, and the collection constructors
   fed a Dict/Set source.

Overall: builtins are among the most rigorously tested areas of the codebase; no correctness bugs
confirmed by static analysis. The one substantive concern is the cross-platform `round` precision
assumption (A11-1).
