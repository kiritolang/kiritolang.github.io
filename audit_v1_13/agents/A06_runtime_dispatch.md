# A06 — Runtime dispatch: operators, call protocol, member access, numeric semantics

Area: `src/kirito/runtime.hpp` (~3529 lines). Focus: `applyBinaryOp`/`applyUnaryOp`, `numericBinary` + Integer/Float fast path, comparisons + `in`/`not in`, call protocol (`applyCall`/`callKw`/`makeMethod`, kwarg binding, defaults at call time, enforcing annotations), member get/set (`evalMemberGet`/`evalMemberSet`), privacy check, `self._super_()` parent view. Excludes per-type method tables (A09/A10) except how methods are dispatched/bound.

Status: COMPLETE.

Overall: the numeric core (`numericBinary`, `compareIntFloat`, wraparound helpers, exact IEEE-754
`==`), the call protocol (`callFull`/`bindArgs`/`applyCall`), privacy (`checkPrivateAccess`), and
super (`SuperValue`) are unusually robust and well-tested. GC rooting across default evaluation is
safe (argument handles stay on the GC-scanned operand stack until `applyCall` returns). No crash/UB
weak spot found — the classic traps (INT64_MIN/-1, div/mod/pow by zero, huge repetition, deep
recursion) are all guarded. Findings below are one real correctness asymmetry, a few
low-severity/diagnostic notes, and coverage gaps.

---

## Findings

### A06-1: `!=` is asymmetric when one instance defines only `_ne_` (no `_eq_`) and the other defines neither

- severity: low-medium
- location: `runtime.hpp:2135-2164` (`applyBinaryOp` Eq/Ne handling) + `runtime.hpp:1563-1572` (`kiEquals` `viaEq`)
- category: correctness / operator dispatch asymmetry
- description: The reflected-operand branch that consults the RIGHT instance's `_ne_`/`_eq_`
  (lines 2151-2161) only fires `if (l.kind() != ValueKind::Instance)` — i.e. only when the LEFT
  operand is not a user instance. When the left operand IS a user instance but lacks the relevant
  dunder, the code falls through to `kiEquals`. `kiEquals` (`viaEq`) tries **`_eq_` on both sides**
  but never consults a standalone **`_ne_`**. So a class that defines only `_ne_` (no `_eq_`) is
  honored when it is on the left of `!=`, but ignored when it is on the right of `!=` and the left
  is another instance without dunders. The `_eq_`-only asymmetry is correctly covered by
  `viaEq(b,a)`; only the `_ne_`-only case is missed. The line-2154/2156 branch shows clear intent to
  support standalone `_ne_`, so this is an incomplete generalization, not a deliberate exclusion.
- failure-scenario:
  ```
  class A:                       # defines neither _eq_ nor _ne_
      var _init_ = Function(self): pass
  class B:
      var _init_ = Function(self): pass
      var _ne_ = Function(self, o): return False   # "I'm never unequal"
  var a = A()
  var b = B()
  # b != a  -> calls b._ne_(a) -> False        (b on the left: honored)
  # a != b  -> structural/identity -> True      (b on the right, a is instance: _ne_ ignored)
  ```
  `a != b` and `b != a` disagree.
- proposed-test: assert `(a != b) == (b != a)` for the above (a instance without dunders, b instance
  with only `_ne_`); also cover `_ne_`-only on the left vs a plain non-instance to confirm the
  existing reflected branch still works.
- proposed-fix: either (a) extend the reflected branch to also fire when `l` is an instance that
  lacks `binOpMethod(op)` (and, for Ne, lacks `_eq_`), or (b) have `kiEquals`/the Ne path also try a
  standalone `_ne_` on either operand. Simplest: in the `op==Ne` fallback, before `kiEquals`, check
  either operand for `_ne_`.
- confidence: high that the path behaves as described; medium that it matters in practice (a class
  with `_ne_` but no `_eq_` is unusual).

### A06-2: Native (`NativeFunction`) parameter annotations are declared but never enforced

- severity: low (info / possible doc mismatch)
- location: `function.hpp:78-102` (`bindArgs` — no annotation check); contrast `runtime.hpp:2036-2038`, `2045-2047` (KiFunction enforces)
- category: annotation enforcement hole (by design, but undocumented distinction)
- description: `NativeParam` carries an `annotation` field and `inspect` renders it, but
  `NativeFunction::bindArgs` performs only fill/arity/duplicate/unknown/missing checks — it does NOT
  call `typeMatches` on the bound values, and there is no return-type check for natives. So a
  signatured native declaring `x: Integer` silently accepts a `String`; each native impl must guard
  its own arg types. CLAUDE.md describes "enforcing type annotations" as a language feature and lists
  native methods among callables that "take optional enforcing type annotations"; the enforcement is
  actually Kirito-function-only. Not a bug (impls do validate), but the asymmetry with KiFunction is
  worth pinning down / documenting.
- failure-scenario: a third-party native declaring `NativeParam("n", "Integer")` whose impl assumes
  an Integer and downcasts without checking would type-confuse on a String arg — the framework does
  not backstop it the way it does for Kirito functions.
- proposed-test: register a signatured native with an `Integer` param, call it with a String, and
  assert the documented behavior (either it is enforced, or the doc is corrected to say native
  annotations are advisory).
- proposed-fix: either enforce annotations in `bindArgs` (call `typeMatches`, needs a `KiritoVM&`
  param — currently `bindArgs` is VM-less) or amend CLAUDE.md/docs to state native annotations are
  advisory (inspect-only).
- confidence: high.

### A06-3: Inconsistent diagnostic for ordered comparison depending on which operand is the String

- severity: low (diagnostic quality)
- location: `runtime.hpp:161-166` (numeric side) vs `runtime.hpp:368` (`StrVal::binary` fallthrough)
- category: error-message consistency
- description: `1 < "a"` dispatches through `IntVal::binary -> numericBinary`, whose guard throws the
  informative `unsupported operand type 'String' for comparison with 'Integer'`. The mirror
  `"a" < 1` dispatches through `StrVal::binary`, falls off the `b.kind()==String` branch, and throws
  the generic `type 'String' does not support this operator` — it names neither the other operand
  nor that this is a comparison. Same for `List < Integer` (`runtime.hpp:457-459` gives a good
  message) but Set/Bytes fallthroughs vary. Minor, but "errors are part of the language".
- failure-scenario: user comparing a String to a number gets a worse message in one operand order
  than the other.
- proposed-test: assert both `1 < "a"` and `"a" < 1` throw messages naming both types and
  "comparison".
- proposed-fix: give `StrVal::binary`'s ordering fallthrough the same `unsupported operand type … for
  comparison with …` shape used by `numericBinary`.
- confidence: high.

### A06-4: Instance whose `_call_` is a native function cannot receive keyword arguments

- severity: low (edge / documented-ish limitation)
- location: `runtime.hpp:1821-1837` (`InstanceValue::callKw`)
- category: call protocol completeness
- description: `callKw` forwards kwargs only when `_call_` resolves to a `ValueKind::Function`
  (KiFunction); if a class's `_call_` is a native function it throws
  `'<class>' _call_ does not accept keyword arguments`. CLAUDE.md says kwargs work "uniformly across
  every callable" including "instance/inherited/`_super_` method calls". A user `_call_` is always a
  KiFunction so this is only reachable if a native class installs a native `_call_` — narrow, but the
  uniformity claim has this seam.
- failure-scenario: a C++ `NativeClass` exposing a native `_call_` used as `obj(k = 1)` throws.
- proposed-test: (if such a native class exists) call it with a keyword; else note as an accepted
  limitation.
- proposed-fix: route a native `_call_` through the signatured/kwarg-aware native dispatch
  (`bindArgs`/`callKw`) instead of rejecting.
- confidence: medium (depends on whether any native class ships a native `_call_`).

### A06-5: `.compare()` collapses distinct large-magnitude Integers via the double round-trip

- severity: low (edge; `.compare` is tolerant by contract)
- location: `runtime.hpp:300-318` (`makeNumericCompare` → `floatClose(asDouble(self), asDouble(other), …)`)
- category: numeric method precision
- description: `.compare` converts both operands with `asDouble`, so two distinct int64s beyond 2^53
  map to the same double and compare "close" even with `abs_tol = 0` and a tiny `rel_tol`
  (`(2**60).compare(2**60 + 1, rel_tol = 0.0, abs_tol = 0.0)` returns True — the exact difference is
  invisible). This is consistent with `.compare` being the *tolerant* comparator (CLAUDE.md: "ONLY
  `==`/`!=` are exact"), and with default rel_tol they'd be close anyway; but `abs_tol=0,
  rel_tol=0` is documented as bit-exact-ish and here it isn't for large ints. Contrast the exact
  `==` path, which routes Integer↔Integer through `IntVal::equals` with no double round-trip.
- failure-scenario: `(2**60).compare(2**60 + 1, rel_tol=0.0, abs_tol=0.0)` -> True (surprising).
- proposed-test: assert the above and decide the intended contract (document that `.compare` is
  double-precision, or special-case Integer↔Integer to compare exactly when both tolerances are 0).
- proposed-fix: optional — when both operands are Integer, compare int64 exactly for the
  `rel_tol==0 && abs_tol==0` case, else keep the double path.
- confidence: high that the behavior occurs; low that it needs changing.

---

## Coverage gaps (existing tests strong; these angles appear untested)

