# A04+A05 — Bytecode VM + Runtime Dispatch (v1.14)

Audit of `src/kirito/bytecode_vm.hpp` and `src/kirito/runtime.hpp`.
Builds on v1.13 A05 (bytecode_vm) + A06 (runtime dispatch). Focus: what they missed + re-verify their fixes.

Status: COMPLETE.

---

## Re-verification of v1.13 fixes

- **A05-1 (GetIter GC-UAF)** — FIXED, code-correct. `bytecode_vm.hpp:304` now declares
  `RootScope rs(vm_)` before the `if (lazy)` branch (hoisted out of the old `else`), the eager branch
  adds every produced item to `rs` (line 312), and `alloc(cursor)` (line 315) executes while `rs` is
  still alive. Since `alloc` runs GC before insertion, the items are now rooted through that window.
  Correct and complete. (Full ASan GC-stress confirmation requires an asan build + `setGcThreshold(1)`,
  which is not scriptable from `ki` — flag for orchestrator to run the proposed A05-1 test under asan.)
- **A09-2 (NaN sort total order in kiLessThan)** — FIXED, code-correct + empirically confirmed.
  `runtime.hpp:403-409`: `x<y iff (isNan(y) && !isNan(x))`, imposing NaN-as-largest total order.
  Verified: `[3.0,nan,1.0,inf,2.0,nan].sort()` -> `[1.0,2.0,3.0,inf,nan,nan]`;
  `min([nan,1.0,2.0])==1.0`; `max([1.0,nan,2.0])==nan`. Strict-weak-ordering holds (NaN equivalent
  only to NaN; distinct from every real incl. +inf). Correct.
- **Bytes ordering branch in kiLessThan** — FIXED, code-correct + empirically confirmed.
  `runtime.hpp:416-418`: `dynamic_cast<BytesVal>` on both, compares `xb->data < yb->data`
  (std::string unsigned-byte memcmp order). Verified sort/min/max over Bytes yields unsigned-byte
  lexicographic order (`[b'\x00', b'\x80', b'\xff']`). Correct.

---

---

## Findings

### A04-1: `_not_` return value is not enforced to be a Bool (inconsistent with `_bool_`; violates documented contract)
- severity: low
- location: `runtime.hpp:2155-2164` (`applyUnaryOp`, the `_not_` dispatch branch at 2159-2161)
- category: correctness / operator-dispatch consistency / doc mismatch
- description: `not x` on an instance defining `_not_` returns **whatever `_not_` returns, unchecked**.
  `applyUnaryOp` does `if (instance has _not_) return o.unary(vm, op, operand);` and `InstanceValue::unary`
  simply calls the user method and returns its result verbatim. Contrast `_bool_`, which the runtime
  **enforces** to be a Bool (`'C'._bool_ must return a Bool, got 'Integer'`). The docs
  (`docs/pages/09-types.md:353`) explicitly say `_not_(self)` yields "a truth value", and CLAUDE.md
  states "every `if`/`while`/`and`/`or`/`not`/`Bool(x)`/`filter` dispatches through `[_bool_]`; must
  return a `Bool`" — implying `not c` is always a Bool. But a class with `_not_` bypasses `_bool_`
  entirely and can return a non-Bool. This breaks the invariant that the logical `not` operator yields
  a Bool, and makes double-negation non-idempotent: `not (not c)` becomes `not <non-bool>` which then
  falls back to `truthy()` of the non-bool, so it does NOT round-trip to a Bool of `c`'s truthiness.
- failure-scenario:
  ```
  class C:
      var _init_ = Function(self): pass
      var _not_ = Function(self): return "x"     # returns a String, not a Bool
      var _bool_ = Function(self): return True
  var c = C()
  # not c            -> "x"     (a String! — surprising; `c and True` correctly returns Bool via _bool_)
  # not (not c)      -> False   (not "x": truthy("x") is True, negated -> False)
  ```
  Verified live: `not c` prints `x`.
- proposed-test: `.experr` asserting `not c` throws `'C'._not_ must return a Bool, got 'String'`
  (if enforcement is the decision), OR a golden documenting that `_not_` is deliberately unchecked
  (like `_neg_`, which legitimately returns arbitrary/symbolic values) + a docs correction to
  09-types.md removing the "truth value" claim.
- proposed-fix: mirror the `_bool_` enforcement in the `_not_` branch of `applyUnaryOp`/`InstanceValue::unary`
  — check the return is a Bool and throw a clear error otherwise. (Decision needed: `_not_` could be
  intended as a symbolic-negation hook like `_neg_`, in which case only the docs need fixing. But
  because `not` is a *logical* operator that feeds `if`/`while`/`and`/`or` and the whole language treats
  it as Bool-producing — unlike `-x`/`_neg_` which is arithmetic — enforcing Bool is the more
  consistent choice.)
- confidence: high that the behavior occurs (verified); medium on whether it should be enforced vs
  documented-as-unchecked.

### A04-2: kiLessThan's cyclic-structure guard reports "equality recursion" for ordering comparisons
- severity: low (diagnostic wording)
- location: `runtime.hpp:394` (`EqualsGuard guard;` inside `kiLessThan`) — the guard's throw message
  lives in `EqualsGuard`/its exceeded-message (`"maximum equality recursion depth exceeded (cyclic
  structure?)"`).
- category: error-message consistency
- description: `kiLessThan` reuses `EqualsGuard` to bound element-wise recursion on deeply-nested /
  cyclic Lists (correct behavior — it throws catchably instead of overflowing the native stack). But
  when the guard trips during an **ordering** comparison (`<`, `sort()`, `min`/`max`), the thrown
  message says "maximum **equality** recursion depth exceeded", which is misleading for a `<`
  operation — the user compared with `<`/`sort`, not `==`. "errors are part of the language".
- failure-scenario: `var a = [1]; a.append(a); var b = [1]; b.append(b); a < b` throws
  `maximum equality recursion depth exceeded (cyclic structure?)` — verified live. The word
  "equality" is wrong for `<`.
- proposed-test: assert the `<`-on-cyclic-list error message reads "comparison"/"ordering" not
  "equality" (once reworded).
- proposed-fix: give `kiLessThan` its own guard message (e.g. "maximum comparison recursion depth
  exceeded (cyclic structure?)"), or make the guard message generic ("maximum recursion depth exceeded
  comparing nested structures").
- confidence: high.

### A04-3 (re-confirm, still OPEN): A05-2 excSpan clobbered by nested try/catch inside a finally
- severity: low
- location: `bytecode_vm.hpp:367` (Reraise reads `excSpan_`), `439` (unwind writes it), `464` (member)
- category: correctness / diagnostic
- description: v1.13 finding **A05-2 was NOT fixed**. Confirmed still present. A reraised (unmatched)
  exception whose `finally` block ran its own inner `try/catch` reports the escaping exception at the
  **inner** throw's line:col, because the single per-frame `excSpan_` slot is overwritten by the
  nested `unwind()`. Flagged for the orchestrator (open v1.13 item, not a re-report of new work).
- failure-scenario (verified live): a program with `throw "OUTER"` (in a try whose catch doesn't
  match) + `finally: try: throw "INNER" catch: pass` reports the uncaught OUTER at the `throw "INNER"`
  line (7:9) instead of the `throw "OUTER"` line (2). See v1.13 A05-2.
- proposed-fix: as v1.13 A05-2 — park/restore `excSpan_` in the compiler's finally-exception spill, or
  carry the span with the parked value.
- confidence: high (reproduced).

### A04-4 (re-confirm, still OPEN): A06-1 `!=` asymmetric for a `_ne_`-only class on the right
- severity: low
- location: `runtime.hpp:2180-2208` (Eq/Ne dispatch) + `runtime.hpp:1592-1601` (`kiEquals` viaEq)
- category: correctness / operator-dispatch asymmetry
- description: v1.13 finding **A06-1 was NOT fixed**. Confirmed still present. When the LEFT operand is
  an instance lacking `_ne_`/`_eq_` and the RIGHT is an instance defining only `_ne_`, the reflected
  branch (which fires only `if (l.kind() != Instance)`) is skipped, so `kiEquals` runs (which consults
  only `_eq_`, never a standalone `_ne_`). Result: `b != a` (b on the left, honored) disagrees with
  `a != b` (b on the right, ignored). Verified live: `b != a` -> `False`, `a != b` -> `True`.
  Flagged for the orchestrator (open v1.13 item).
- proposed-fix: as v1.13 A06-1 — in the Ne fallback, before `kiEquals`, consult a standalone `_ne_` on
  either operand (or extend the reflected branch to also fire when `l` is an instance missing the
  dunder).
- confidence: high (reproduced).

### A04-5: coverage — NaN min/max order-independence (a consequence of the A09-2 fix) untested
- severity: coverage-gap
- location: `runtime.hpp:3170-3181` (min/max `extremum`), relies on `kiLessThan`'s NaN total order
- category: coverage
- description: The A09-2 fix (NaN sorts as largest in `kiLessThan`) has a good C++ regression test for
  `sorted` (`test_audit_v113.cpp:201`). A *valuable* side-effect it also produced — `min`/`max` over a
  list containing NaN are now **order-independent** (`min([nan,1.0])==min([1.0,nan])==1.0`,
  `max([1.0,nan])==max([nan,1.0])==nan`) because both directions route through the same total order —
  is NOT asserted anywhere. Before the fix, min/max results depended on element order (NaN "poisoned"
  whichever slot it landed in as `best`). A regression here would be silent.
- proposed-test: `.ki`: `assert min([nan, 1.0, 2.0]) == 1.0 and min([1.0, 2.0, nan]) == 1.0`;
  `assert max([1.0, nan, 2.0]).compare(...)`-style NaN check for max returning NaN in both orders;
  plus `sorted([1.0, nan, 2.0], reverse=True)` puts NaN first (verified live: `[nan, nan, 3.0, 2.0, 1.0]`).
- confidence: high (gap).

### A04-6: coverage — Bytes ordering in kiLessThan (sort/min/max) has a `.ki` test but no C++ test
- severity: coverage-gap
- location: `runtime.hpp:416-418` (Bytes branch); `.ki` coverage in
  `tools/tests/scripts/verify_types_bytes.ki:95-98`
- category: coverage (C++/`.ki` parity — this round's explicit goal)
- description: The Bytes branch of `kiLessThan` (unsigned-byte lexicographic sort/min/max over Bytes)
  is well-covered on the `.ki` side (`verify_types_bytes.ki`) but the C++ unit suite
  (`test_audit_v113.cpp`) only exercises Bytes **iteration** GC-stress (line 42) and Bytes serde bucket
  (207+), not sort/min/max ordering. Per v1.14's C++/`.ki` parity goal, add a C++ assertion so the
  Bytes-ordering path is pinned in both languages.
- proposed-test: C++ `CHECK(ok("String(sorted([Bytes([200]), Bytes([100]), Bytes([1,0]), Bytes([1])]))") == ...)`.
- confidence: high (gap).

### A04-7: (Positive) A05-1 lazy-iterator branch is also GC-safe — confirmed
- severity: non-bug (recorded positive)
- location: `bytecode_vm.hpp:305-307` (lazy branch) + `object.hpp:118`, `stdlib_io.hpp:125-138`
- category: memory (verified sound)
- description: While re-verifying the A05-1 fix I checked the LAZY branch of `GetIter`, which the fix's
  `RootScope rs` does NOT populate (only the eager branch calls `rs.add`). This is safe: the only
  `lazyIterate` overrides (File `LineIter`, BytesIO) hold **only** `self` (= `cursor->source`, which is
  set before `alloc(cursor)` and kept rooted because `iterable` is still `peek(0)` on the operand
  stack, and `IterCursor::children()` re-emits `source` when `lazy` is set). No intermediate handle is
  materialized that would be unreachable during the `alloc(cursor)` GC. Recorded so a future
  `lazyIterate` override that allocates internal state is reviewed against this invariant.
- confidence: high.

### Verified sound (no finding) — additional angles checked this round
- Numeric semantics: floor-div/mod sign with negatives (`-7//2==-4`, `7%-2==-1`), float mod
  (`-7.5%2==0.5`), `**` right-assoc (`2**3**2==512`), int64 wraparound (`IMAX+1`), `INT64_MIN//-1` and
  `%-1` (no UB), `-(INT64_MIN)` wraps, all zero/negative-power/fractional-negative-base domain throws.
- Exact int/float `==` + hashing at the 2^53 / 2^63 boundary: `9007199254740993 == 9007199254740992.0`
  is False and not in `{...992.0}`; `IMAX != 2^63-as-float` but `IMAX < 2^63`. (A06-6 gap behavior is
  correct; still untested — see A06-6.)
- Call protocol: default referencing earlier param (`b=a+1`), mutable-default freshness (fresh `[]`
  per call), keyword out-of-order, duplicate-keyword/unknown-keyword/positional-after-keyword/
  too-many/missing-required errors — all correct.
- Privacy: sibling-class rejection (B1 method can't read B2 private), subclass-reads-base-private
  allowed, multi-level `_super_()` chains compose (init and method).
- Exceptions: throw-in-finally overrides body throw; return-in-finally swallows exception;
  continue/break in try/finally/with run cleanup correctly; `_exit_` runs on break; operand stack
  stays clean after caught mid-expression exceptions (index/div-by-zero in a loop).
- Guards: recursive `_eq_` -> call-depth guard; cyclic list `==`/`<` -> EqualsGuard (catchable);
  cyclic stringify -> `[...]` marker (no overflow).
- Cross-type `==` symmetry: `2 == Complex(2,0)` and `Complex(2,0) == 2` both True via kiEquals's
  cross-kind `ob.equals(oa)` fallback.
- `_exit_` takes only `self` (NOT Python's 3-arg exc-info) — by design; the compiler emits a 0-arg
  `_exit_()` call. Not a bug (a user `_exit_(self,a,b,c)` correctly errors "missing argument 'a'").
- Switch dispatch: type+value-exact match (`case 1` != `case 1.0` != `case "1"` != `case True`),
  non-scalar/NaN subject -> default, `-0.0`/`0.0` same key.
- `_bool_` non-Bool return correctly enforced with the condition's real span (A05-4 fix works).
- Reflected arithmetic correctly does NOT reflect (`3 * v` with `v._mul_` throws; `v * 3` works).
- numericBinary non-numeric guard reachable via `pow`/`divmod` throws cleanly (A06-10 behavior).

---

## Summary

Re-verified all three v1.13 fixes named in the brief — **A05-1 (GetIter GC-UAF)**,
**A09-2 (NaN sort total order)**, and the **Bytes-ordering branch in kiLessThan** — as correct and
complete (code-checked + empirically confirmed; A05-1's lazy branch also verified safe, A04-7).
The bytecode VM + runtime-dispatch subsystem is in excellent shape; extensive adversarial probing
(numeric overflow/sign/domain, call/keyword binding, privacy, super chains, exception unwinding
across finally/with, cyclic-structure guards, switch dispatch, GC-rooting windows) found no new
crash/UAF.

New findings this round:
- **A04-1** (low): `_not_` return value not enforced to Bool — inconsistent with `_bool_` and the
  documented "truth value" contract; `not c` can return a String. Decision needed (enforce vs
  document-as-unchecked).
- **A04-2** (low): kiLessThan's cyclic-guard error says "equality recursion" for `<`/sort — wrong word.

Re-confirmed still-OPEN v1.13 findings (flagged for orchestrator, not re-reported as new work):
- **A04-3 / v1.13 A05-2** (low): excSpan clobbered by nested try in finally — reproduced.
- **A04-4 / v1.13 A06-1** (low): `!=` asymmetric for `_ne_`-only class on the right — reproduced.

Coverage gaps (v1.14 C++/`.ki` parity focus):
- **A04-5**: NaN min/max order-independence (a beneficial side-effect of A09-2) untested.
- **A04-6**: Bytes-ordering-in-kiLessThan tested in `.ki` only, no C++ assertion.
