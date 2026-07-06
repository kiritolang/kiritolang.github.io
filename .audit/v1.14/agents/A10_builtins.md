# A10 — Builtins Audit (Kirito v1.14)

Area: builtin function surface (`installBuiltins` in `runtime.hpp`) + type constructors/converters
`Integer`/`Float`/`String`/`Bool`/`List`/`Set`/`Dict`/`Bytes`.

Status: **complete**.

Method: static read-only scan of `src/`, plus throwaway `.ki` probes confirmed against
`build-debug/ki`. Prior findings (v1.13 `A11_builtins.md`, v1.12) merged — hunting NEW angles.

Builtin *functions* registered in `KiritoVM::installBuiltins()` (`runtime.hpp:2807+`). `builtins.hpp`
holds only the value classes + UTF-8/float helpers.

---

## Overall assessment

This subsystem is **exhaustively tested and robust**. Every fresh angle I probed against
`build-debug/ki` behaved correctly and, for almost all, already had a golden `.ki` or C++ test:
`Integer("0xFF")`/`Integer("  12  ")`/`Integer(-3.9)`(→-3, trunc-toward-zero)/`Integer("")`(throw)/
`Integer(inf)`(throw); `Float("inf")`/`Float("nan")`/`Float("0x1p4")`(throw); `Integer(True)`/`Float(True)`;
`round(0.5/2.5/3.5/-2.5)` (half-away-from-zero, not banker's — pinned in r4/r6/r7/r9/test_audit_v113);
`round(2.675,2)==2.67`, `round(12345.0,-2)==12300.0`; `range` float-arg/zero-step/huge (all throw/guard);
`sum` start (pos+kw), empty, INT64 wrap (→INT64_MIN); `min/max` key/default/empty/mixed-uncomparable;
`zip` unequal/empty; `enumerate` empty; `divmod` floats/negatives/`INT64_MIN,-1`; `pow` neg-base-frac
(throw)/modular neg-exp(throw)/mod=1(→0); `abs`(INT64_MIN wrap)/Bool-reject; `bin/oct/hex` of
zero/negative (sign-magnitude `-0xff`); `bitand/bitor/bitxor/bitnot/shl/shr` on negatives + huge/neg
shift counts; `ord`(multi-char/empty throw)/`chr`(neg/>0x10FFFF/surrogate throw); `filter/all/any`
`_bool_` dispatch; multi-key `sorted`; `List/Set/Dict` from Dict/Set sources; `Bytes` from
List/Integer/String + out-of-range element (throw); `isinstance` type-name String; `hasattr` on a
None-valued attr (True) and on a class value (False).

Two prior-cycle findings are now RESOLVED and one gap CLOSED (see "Resolved / closed" below). Net-new
material this cycle is limited to one small coverage/design gap plus three still-open carry-forwards.

## Findings

### A10-1: builtin `abs()` / `round()` reject Complex — arguable design asymmetry + untested
- severity: Low
- location: `src/kirito/runtime.hpp:2997-3007` (`abs`), `runtime.hpp:3008-3052` (`round`)
- category: weak-spot / coverage-gap
- description: `abs` handles only `ValueKind::Integer`/`Float` and otherwise throws
  `"abs expects a number"`; `round` likewise (`isNumeric(xo)` is false for a Complex → `"round expects
  a number"`). But CLAUDE.md classes `Complex` as one of "every native numeric type", `abs`'s declared
  signature is `{"x", "Number"}`, and Complex carries a natural magnitude (`.modulus()`, exposed as the
  module function `complex.abs` which IS tested at `r10_numeric.ki:382`). So the *builtin* `abs`/`round`
  silently exclude a type the docs call numeric, while a parallel module function covers only `abs`.
  Confirmed live: `abs(complex.Complex(3,4))` → THROW `"abs expects a number"`;
  `round(complex.Complex(3,4))` → THROW `"round expects a number"`.
- failure-scenario: A user reasonably writes `abs(z)` for a Complex `z` (as in Python) and gets a
  type error rather than the modulus; the asymmetry (`complex.abs(z)` works, `abs(z)` doesn't) is
  surprising and undocumented.
- proposed-test: at minimum a regression pinning the *current* behavior —
  `assert throws(Function(): return abs(complex.Complex(3,4)))` and same for `round` — so a future
  change is deliberate; better, decide+document whether builtin `abs` should delegate to a Complex's
  modulus (dispatch through a `_abs_`-style slot / the object protocol) and add the positive test.
- proposed-fix: either (a) document that builtin `abs`/`round` are real-scalar-only and Complex uses
  `complex.abs`, or (b) route `abs` through the value's own magnitude slot so Complex (and any future
  NativeClass numeric) participates. No code change proposed without a design decision.
- confidence: High (behavior confirmed live; no test found exercising builtin `abs`/`round` on Complex).

### A10-2: `hasattr` internal probe catches only `KiritoError`; a native `getAttr` throwing a plain `std::exception` escapes  [carry-forward v1.13 A11-8, STILL OPEN]
- severity: Low
- location: `src/kirito/runtime.hpp:3354-3359`
- category: weak-spot
- description: Unchanged since v1.13. `hasattr` probes `getAttr` inside `try { ... } catch (const
  KiritoError&)`. In-tree types only throw `KiritoError`, but a third-party `NativeClass::getAttr` (the
  advertised extension surface) that throws a `std::runtime_error`/`std::out_of_range`/`std::bad_alloc`
  would propagate as an uncaught C++ exception past the intended boolean, unlike a bare Kirito `catch`
  which does absorb `std::exception`.
- failure-scenario: `hasattr(thirdPartyObj, "x")` where the object's `getAttr` throws a non-`KiritoError`
  std exception → escapes instead of returning `False`.
- proposed-test: a `NativeClass` test double whose `getAttr` throws `std::runtime_error`, asserting
  `hasattr(obj, "missing") == False` (not a crash).
- proposed-fix: broaden the probe to `catch (const std::exception&)` (or `catch (...)`), consistent with
  Kirito's boundary policy that a native throw becomes a clean result.
- confidence: Medium (theoretical — no in-tree native currently does this).

### A10-3: `NativeFunction::bindArgs` does NOT enforce `NativeParam::annotation` — advisory-only for natives  [carry-forward v1.13 A11-3, STILL OPEN]
- severity: Low
- location: `src/kirito/function.hpp:78-102` (`bindArgs`)
- category: weak-spot
- description: Unchanged since v1.13. `bindArgs` binds positionals/keywords and fills defaults but never
  consults `sig_[i].annotation`. So the declared types on signatured builtins (`abs`'s `"Number"`,
  `chr`'s `"Integer"`, `bin`'s `"Integer"`, etc.) are shown by `inspect` but do not gate the call — every
  impl re-validates itself. This diverges from Kirito-defined functions, whose annotations ARE
  runtime-enforced via `typeMatches`. Latent trap: a newly-added signatured native that trusts its
  annotation and omits its own kind-check + downcasts is type-confusion UB.
- failure-scenario: contributor adds `defSig("twice", {{"x","Integer"}}, "Integer", ...)` doing
  `static_cast<const IntVal&>(o).value()` with no kind check; `twice(3.5)` reinterprets a FloatVal as
  IntVal instead of throwing.
- proposed-test: assert a signatured native with a mismatched positional does NOT throw from the binder
  (documents current behavior); or make `bindArgs` optionally enforce via `typeMatches`.
- proposed-fix: either enforce annotations in `bindArgs` (matching Kirito-fn semantics) or add a
  prominent `NativeParam::annotation`-is-advisory-only comment; audit confirms all current impls self-check.
- confidence: High (behavior confirmed from source); severity Low (all current impls self-validate).

### A10-4: Integer→base-digits loop duplicated between `radix` (bin/oct/hex) and `applyFormatSpec`  [carry-forward v1.13 A11-2, STILL OPEN]
- severity: dry
- location: `src/kirito/runtime.hpp:3388-3394` (`radix` lambda) vs the integer branch of `applyFormatSpec`
  (runtime.hpp ~2707-2714) and the inverse-direction loop in `Integer(String)`.
- category: dry
- description: Unchanged since v1.13. The "negate-to-unsigned, emit `alpha[u % base]` digits, reverse"
  loop appears in `radix` and again in the format integer path, with the same `"0123456789abcdef"`
  alphabet. A future base-formatting change (new base, sign fix) must touch both or `hex(n)` and
  `format(n,"x")` drift for an edge like INT64_MIN.
- proposed-test: property test asserting `hex(n)`/`oct(n)`/`bin(n)` agree with `format(n,"x"/"o"/"b")`
  (modulo the `0x`/`-` prefix) over random n including INT64_MIN.
- proposed-fix: extract `intToBaseDigits(uint64_t u, int base, bool upper) -> std::string` and call it
  from both.
- confidence: High.

---

## Resolved / closed since v1.13

- **v1.13 A11-1 (round long-double portability) — RESOLVED.** `round(x, ndigits)` now guards the
  extended-precision scaling with `#if LDBL_MANT_DIG > DBL_MANT_DIG` and falls back to a
  `snprintf("%.*f")` + `strtod` correctly-rounded path (nd>=0) / plain-double scaling (nd<0) where
  `long double == double` (MSVC). See `runtime.hpp:3027-3045`. The prior cross-platform discrepancy is
  fixed.
- **v1.13 A11-7 (List/Set/Dict constructors from a Dict/Set source untested) — CLOSED.** Now covered:
  `r9_builtins_err.ki:100-102` (List(empty set), List({}) yields keys, List(dict) yields keys),
  `r10_types.ki:719-720`, `r10_builtins.ki:123`, `spec_builtins.ki:68`.
- **v1.13 A11-4/A11-5/A11-6 (divmod edges / range positional-vs-keyword / sum wrap) —** the divmod
  `INT64_MIN,-1` and sum-wrap cases confirmed correct live this cycle; if the specific asserts were
  added they are now redundant, otherwise still minor gaps (low value — behavior verified correct).

## Non-findings (verified intentional / already covered)

- **min/max silently ignore `default=` given multiple positional args** (`min(5,3,8,default=0)` → 3):
  this is **intentional and explicitly tested** — `r8_builtins.ki:207-208` ("min/max ignores default
  with several positionals"), `r10_builtins.ki:317`. Differs from Python (which raises) BY DESIGN. Not
  a finding.
- **round half-away-from-zero** (not banker's): documented and pinned across r4/r6/r7/r9 +
  `test_audit_v113.cpp:268-269`.
- **isinstance has no tuple/list-of-types form** (`isinstance(1,[Integer,Float])` throws): a clean
  error, consistent with the single-type contract in CLAUDE.md; not a defect.

## Coverage: C++ vs `.ki`

The builtin surface is driven almost entirely from `.ki` golden scripts (`audit_builtins.ki` 849 lines,
`r4_`/`r6_`/`r7_`/`r8_`/`r9_`/`r10_` builtin/number families, `spec_kwargs.ki`, `probe_kwargs.ki`) with
C++ unit backup (`test_builtins_deep.cpp`, `test_audit_*.cpp`, `test_domain_guards.cpp`,
`test_hardening2.cpp`, `test_introspect.cpp`). Gaps flagged above (all Low/minor):
- builtin `abs`/`round` **on Complex** — untested (A10-1).
- `hasattr` against a native `getAttr` that throws a **non-`KiritoError`** — untested (A10-2), requires a
  C++ test double (can't be reached from pure `.ki`).
- `bindArgs` **annotation non-enforcement** — no test documents it (A10-3).

## Summary

Counts (net-new + still-open carry-forwards):
- Critical/High: 0 (no confirmed correctness bugs)
- Medium: 0
- Low: 3 — A10-1 (abs/round reject Complex; asymmetry+gap), A10-2 (hasattr catch scope, carry-fwd),
  A10-3 (bindArgs annotation non-enforcement, carry-fwd)
- dry: 1 — A10-4 (base-digit loop duplication, carry-fwd)

Resolved since v1.13: A11-1 (round long-double portability) fixed via `#if LDBL_MANT_DIG`. 
Closed gap: A11-7 (constructors from Dict/Set) now tested.
Non-findings: min/max default-with-multi-positionals ignore is INTENTIONAL+tested; round half-away-from-zero tested.

Assessment: builtins + type constructors are correct and exhaustively tested. No new correctness bugs;
only one small design/coverage gap (A10-1) and three pre-existing low/dry items still open.
