# A06 — Object model audit (v1.14)

Scope: object.hpp, class_value.hpp, instance_value.hpp, function_value.hpp, module_value.hpp,
plus the runtime.hpp instance-slot bodies (dunder dispatch, privacy, _super_, attr get/set,
method binding, class-attr copy-on-write). Read-only on src/.

STATUS: in progress

Prior context: v1.13 A07 found A07-1..13 (iter self-recursion crash, findMethod recursion,
str depth, VM-less equals inconsistency, _str_ not type-checked, bound-method identity,
plus coverage gaps). Hunting NEW angles for v1.14.

---

### A06-1: `InstanceValue::str` has a cycle guard but NO depth bound → deep `_str_` delegation segfaults (uncatchable)
- severity: medium
- location: src/kirito/runtime.hpp:1913 `InstanceValue::str` (the `StringifyCtx` carries a `depth` field that this slot never increments/checks)
- category: correctness / resource (uncatchable native-stack overflow)
- description: `str()` protects a SELF-referential `_str_` with `ctx.active`, but if `_str_` returns a *different* instance (line 1922: non-String return → `ctx.active.insert(this); o.str(ctx)`), the next instance's `str` recurses in native C++. `ctx.depth` is declared for exactly this (object.hpp:34) but `InstanceValue::str` never touches it — only container stringification bounds depth. A single class whose `_str_` returns the next node in a linked chain overflows the C++ stack.
- failure-scenario: CONFIRMED SIGSEGV (exit 139). `class Node: _init_(self,nxt): self.nxt=nxt; _str_(self): return self.nxt` — build a 200k-deep chain, then `String(head)` segfaults. A 20k chain still returns cleanly, so it's pure native-stack depth. Construction of 200k nodes alone is fine — the crash is the str recursion.
- note: v1.13 filed this as A07-3 at LOW ("needs many distinct classes"); it is in fact reachable with ONE class and ordinary deep data, and is an uncatchable crash — re-rating medium.
- proposed-test: adversarial script asserting a deep `_str_` chain throws a catchable error (not a crash); a CTest under asan.
- proposed-fix: increment/check `ctx.depth` in `InstanceValue::str` (throw a catchable KiritoError past a bound), exactly as container stringification already does.
- confidence: high (reproduced)

### A06-2: `_str_` return value is not type-checked (spec says it returns a String) — silent leniency
- severity: low
- location: src/kirito/runtime.hpp:1922
- category: correctness (spec deviation) / coverage
- description: `_bool_` must return Bool, `_hash_` Integer, `_len_` a non-negative Integer — all enforced. `_str_` per CLAUDE.md "returns a String" but a non-String return is silently recursively stringified (`o.str(ctx)`), so `_str_` returning a List yields its repr instead of a clear type error. (Still present from v1.13 A07-5; combined with A06-1 the non-String return is also the crash vector.)
- proposed-fix: decide the contract — enforce String or document the leniency — and add a test.
- confidence: high

### A06-3: class-attr mutation leaks across instances; assignment is copy-on-write (documented footgun, untested)
- severity: coverage-gap
- location: src/kirito/runtime.hpp:1714 `InstanceValue::getAttr` (class-var returned as the shared handle) + :117 `setAttr` (writes to instance `attrs`)
- category: coverage
- description: CONFIRMED — a class-body `var shared = []` is one shared handle; `a.shared.append(1)` mutates the object every instance sees (`id(a.shared)==id(b.shared)` is True after both mutate). Only *assignment* (`a.shared = ...`) copies-on-write into the instance's own attrs. This matches CLAUDE.md ("copy-on-write on assignment") and Python's class-attr footgun, but I found no script/CTest pinning either half.
- proposed-test: assert mutation of a mutable class attr leaks (shared identity) while assignment shadows (per-instance).
- confidence: high (reproduced)

### A06-4: a native function stored as a class member and bound as a method drops keyword args and skips `bindArgs`
- severity: low
- location: src/kirito/runtime.hpp:1697 `makeBoundMethod` (native branch `return m.call(vm, full);`)
- category: correctness (edge) / consistency
- description: A class-body `var f = <native fn>` binds as a method (receiver prepended) — see v1.13 A07-13. When invoked, `makeBoundMethod`'s lambda calls `m.call(vm, full)` for the native branch, which is `NativeFunction::fn_(vm, args)` — it bypasses `bindArgs` (so defaults/kwargs are NOT bound) AND silently drops the `named` span. So `inst.storedNative(k = v)` ignores `k = v`, unlike a direct `storedNative(k = v)` call (which routes through `applyCall` → `bindArgs`/`callKw`). Reachable only via the unusual "store a native as a class member" pattern; a Kirito-defined method (the normal case, KiFunction branch) forwards kwargs correctly.
- proposed-test: store a signatured native in a class, call as method with a keyword; assert it errors rather than silently dropping (or is bound).
- proposed-fix: in the native branch, route signatured natives through `bindArgs`/`callKw` (mirror `applyCall`), or reject kwargs with a clear error.
- confidence: medium (static; not run — the pattern is niche)

### A06-5: `ClassValue::findMethod` is still the only chain walker that RECURSES (A07-2 unfixed) — practically unreachable
- severity: low
- location: src/kirito/class_value.hpp:71
- category: resource (theoretical)
- description: `findMethod` recurses through `base`; every sibling walker (`isInstanceOf`, `classIsSubclassOf`, `typeMatches`, inspect) is an iterative `while`. A 15000-deep linear inheritance chain resolves a missing attr cleanly (tested — `catch` succeeds, no overflow); the frame is tiny so overflow needs ~100k+ levels, which is too slow to even parse. So it is effectively unreachable — noting only that the inconsistency from v1.13 A07-2 persists.
- proposed-fix: rewrite as an iterative loop for consistency (cheap, removes the theoretical cliff).
- confidence: high (behaviour), low (reachability)

## Coverage gaps

### A06-6: no test for `_str_` returning a non-String, nor for a deep `_str_` delegation chain (the A06-1 crash)
- severity: coverage-gap
- location: tools/tests/errors/ (has bool_dunder_return_*, class_hash_wrong_type, len_no_length, iter_non_iterable_class, super_no_base) but NO `str_dunder_*`
- description: The dunder return-type suite covers `_bool_`/`_hash_`/`_len_`/`_iter_` but omits `_str_`. There is no assertion that a non-String `_str_` return is handled, and no adversarial test for a deep `_str_` chain — which A06-1 shows is a live segfault. `test_audit_v113.cpp` covers the `_iter_`-returns-self fix (A07-1) but not the analogous `_str_` case.
- proposed-test: (1) `_str_` returning a List → assert documented behaviour; (2) a deep `_str_`-returns-next-instance chain → assert a catchable error (under asan).

### A06-7: no test pinning class-attribute copy-on-write-on-assignment vs shared-mutation semantics
- severity: coverage-gap
- location: covered behaviour in InstanceValue getAttr/setAttr; no script asserts it
- description: See A06-3 — mutation of a mutable class attr leaks across instances (shared handle) while assignment shadows per-instance. `spec_classes.ki`/`classes.ki` don't pin this documented footgun. A regression test would catch any accidental change to the shared-handle semantics.
- proposed-test: assert `id(a.shared)==id(b.shared)` after class-attr mutation, and per-instance shadowing after assignment.

### A06-8: `with`-instance `_enter_`/`_exit_` on a USER class, and `_exit_` throwing on the exception path, are undertested
- severity: coverage-gap
- location: src/kirito/compiler.hpp:527 (with lowering: `_exit_()` called with zero args, return discarded, cannot suppress)
- description: The stdlib context managers (io/net/parallel/tensor) are native. A pure-Kirito `class` with `_enter_`/`_exit_` used in `with` — especially the case where `_exit_` itself throws on the exception unwinding path (the original exception is then replaced, not chained) — is a semantic worth a script test. `_exit_` receives no exception info by design (unlike Python).
- proposed-test: user-class `with` happy path + `_exit_`-throws-during-exception path; assert which error surfaces.

## Notes (non-bugs, confirmed behaviours)
- Operator dunders are resolved on the CLASS chain only (findMethod), never on instance attrs — so
  an instance-attr `_add_` does not affect `c + c` (Python-consistent). Confirmed.
- Reflected `==`/`!=` (`5 == c`, two-instance mixed-`_eq_`) works via BOTH applyBinaryOp and kiEquals
  (`viaEq(a,b)` then `viaEq(b,a)`), so it is symmetric; direct `_ne_` is dispatched (not derived). Tested.
- Privacy: correctly allowed within the class CHAIN (either direction), through `_super_`, blocked for
  unrelated classes and module scope. SetAttr enforces privacy symmetrically with GetAttr. Confirmed.
- `_super_`: multi-level chains climb one level per call (C+B+A), no-base throws, overridable. Confirmed.
- Reentrant `_hash_` self-inserting into the live Set, and a `_eq_` allocating heavily (GC pressure)
  during a Dict lookup, both complete cleanly (ProbeScope + RootScope hold). v1.13 A07-6 residual OK.
- A07-1 (`_iter_` returns self) fix is present and tested (test_audit_v113.cpp). Confirmed.

## Summary
- medium: 1 (A06-1 deep-`_str_` chain segfault, uncatchable — re-rated up from v1.13 low)
- low: 3 (A06-2 `_str_` return not type-checked; A06-4 native-member-method kwarg drop; A06-5 findMethod recursion)
- coverage-gap: 4 (A06-3 class-attr sharing; A06-6 `_str_` tests; A06-7 class-attr COW test; A06-8 user-class `with`)
- Non-bugs confirmed solid: operator/reflected-eq dispatch, privacy, `_super_`, reentrant hash/eq.

STATUS: complete
