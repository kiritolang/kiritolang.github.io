# A07 — Object model audit (object.hpp, class_value.hpp, function.hpp)

Scope: the base `Object` protocol, `ClassValue`/`InstanceValue`/`SuperValue`, `NativeFunction`/`KiFunction`,
and every special/dunder method dispatch (defined in `runtime.hpp`, which owns the instance-slot bodies).
Read-only static audit. Confirmed bugs first, then weak spots, then coverage gaps, then DRY.

## Surface map (for reference)
- `Object` (object.hpp): virtual slots `truthy/str/equals/hashable/hash/children/inspectMembers` and
  operation slots `binary/unary/call/getAttr/setAttr/getItem/setItem/slice/iterate/lazyIterate/length/contains`.
  Each op slot defaults to a clear "unsupported" throw. GC roots exposed via `children()`.
- `ClassValue`: `methods` map (holds methods AND non-function class-body vars = shared class attrs), `base`/`hasBase`,
  `selfHandle`. `findMethod` walks base chain (RECURSIVE). `equals` = identity. `children` = method values + base.
  `call`/`callFull` (runtime.hpp) instantiate → `_init_`.
- `InstanceValue`: `cls`, `selfHandle`, `className`, cached `hasHashDunder/hasEqDunder/hasBoolDunder`, `attrs`.
  Slot bodies in runtime.hpp: `truthy`(→`_bool_`), `str`(→`_str_`), `equals`(→`_eq_`), `hash`(→`_hash_`),
  `binary`(→`_add_`.._ge_`/`_ne_`), `unary`(→`_neg_`/`_not_`), `call`/`callKw`(→`_call_`),
  `getItem`/`setItem`(→`_getitem_`/`_setitem_`), `length`(→`_len_`), `contains`(→`_contains_`),
  `iterate`(→`_iter_`), `getAttr` (attr table → class method → bound-method).
- `SuperValue`: proxy; `getAttr` resolves from `startClass` (base of running method's class), binds to real instance.
- `NativeFunction`: 3 ctors (bare / signatured / kwarg-variadic); `bindArgs` binds pos+named→positional.
- `KiFunction`: `def_` (AST, VM-owned, not GC), `closure_` (GC root), `ownerClass`, `sourceFile`;
  `callFull` binds pos+named+defaults+annotation enforcement.

---

### A07-1: `_iter_` returning `self` (or a mutually-referential instance) causes unbounded native recursion → crash
- severity: medium
- location: `src/kirito/runtime.hpp` `InstanceValue::iterate` (~1860-1867)
- category: bug / weak-spot (uncatchable crash)
- description: `iterate()` invokes `_iter_`, then does `return vm.arena().deref(r).iterate(vm)` on the RESULT.
  If `_iter_` returns the instance itself (a natural mistake for a Python migrant expecting an iterator-returns-self
  protocol — Kirito has no `_next_`), `r` is `self`, so `iterate()` re-enters itself in C++. Each level runs a full
  `_iter_` method call which enters/exits `CallGuard` **balanced** (returns before the recursive `iterate`), so the
  call-depth guard is NEVER exceeded and never trips. The native stack grows one `iterate`+`invokeOp`+`callFull`
  frame per level → stack overflow (SIGSEGV), not a catchable `KiritoError`. Mutually-referential `_iter_`
  (A._iter_→B, B._iter_→A) triggers the same.
- failure-scenario: `class C:\n    var _iter_ = Function(self): return self\nfor x in C(): pass` → segfault.
- proposed-test: adversarial script/CTest asserting `for x in C()` (with `_iter_` returning self) throws a
  catchable error (not a crash); likewise a self-returning result via `List(C())`.
- proposed-fix: cap the `_iter_`-result re-dispatch — either forbid the result being another `Instance` whose
  `iterate` would re-enter, or thread a small depth counter / `EqualsGuard`-style RAII bound around
  `InstanceValue::iterate`, throwing a clear error. Note `InstanceValue::str` already has an analogous cycle guard
  (`ctx.active`); `iterate` has none — inconsistent.
- confidence: high (static trace of CallGuard balance is unambiguous)

### A07-2: `ClassValue::findMethod` is recursive over the base chain → deep inheritance can overflow the stack
- severity: low
- location: `src/kirito/class_value.hpp:68-73` (`findMethod`)
- category: weak-spot
- description: `findMethod` recurses through `base` (`return static_cast<const ClassValue&>(...).findMethod(...)`).
  Every attribute access, operator dispatch, hash, equals, and truthiness on an instance calls it. A pathologically
  deep linear inheritance chain (thousands of sequential `class X(Prev):` defs — not source-nested, so no parser
  nesting guard applies) overflows the C++ stack on the first missing-name lookup. All the sibling walkers
  (`typeMatches`, `isInstanceOf`, `classIsSubclassOf`, inspect) are iterative `while` loops; only `findMethod` recurses.
- failure-scenario: generate 100k-deep inheritance, then a lookup of a name absent from every level → crash.
- proposed-test: build a deep chain and confirm a missing-attr lookup errors cleanly (or is bounded).
- proposed-fix: rewrite `findMethod` as an iterative loop over `base`, matching the other chain walkers.
- confidence: medium (pathological input; real but very unlikely in practice)

### A07-3: `InstanceValue::str` guards cycles but not depth for acyclic `_str_` delegation chains
- severity: low
- location: `src/kirito/runtime.hpp` `InstanceValue::str` (~1868-1885)
- category: weak-spot
- description: `str` protects self-referential `_str_` via `ctx.active` but has no depth bound. If `_str_` returns a
  *different* instance whose `_str_` returns yet another (a long acyclic chain of distinct objects), `o.str(ctx)`
  recurses without hitting the `active` guard. `StringifyCtx` carries a `depth` field intended for exactly this, but
  `InstanceValue::str` never increments/checks it (only container stringification does). Distinct from A07-1 in that
  it needs many distinct classes, so lower risk.
- failure-scenario: N chained classes each `_str_`-returning the next instance → deep recursion at `String(head)`.
- proposed-test: chained-`_str_` script asserting a bounded, catchable outcome.
- proposed-fix: increment/check `ctx.depth` in `InstanceValue::str` (throw past a bound), as containers already do.
- confidence: medium

### A07-4: `InstanceValue::equals` silently returns `false` with no active VM while `hash()` throws — inconsistent
- severity: low
- location: `src/kirito/runtime.hpp` `InstanceValue::equals` (~1782), `hash()` (~1744)
- category: bug / consistency
- description: When `KiritoVM::activeVM()` is null, `equals` returns `false` (after the identity short-circuit),
  but `hash()` throws "requires an active interpreter context". A Dict/Set operation touching an instance key with
  no active VM (e.g. during teardown, or a host manipulating a container off the VM's own thread) would get
  asymmetric behaviour: hashing throws but equality lies "not equal". Also, under `hasEqDunder` the method could
  have been rebound away; `equals` then returns `false` (treats unequal) whereas the truthful answer is unknown.
  Identity equality still works VM-less (the `this == &other` short-circuit), so scalars/Dict internals are safe.
- failure-scenario: value-equality of two structurally-equal instances evaluated with no active VM → wrongly `false`.
- proposed-test: hard to reach from pure Kirito; an embed-API test dereferencing an instance-keyed Dict without a
  running interpreter context.
- proposed-fix: make the two paths consistent (both throw, or both fall back to identity) and document the contract.
- confidence: medium (behaviour confirmed; reachability is narrow)

### A07-5: `_str_` return value is NOT type-checked (unlike `_bool_`/`_hash_`/`_len_`)
- severity: low
- location: `src/kirito/runtime.hpp` `InstanceValue::str` (~1876-1881)
- category: bug (spec deviation) / coverage
- description: `_bool_` must return Bool, `_hash_` must return Integer, `_len_` must return a non-negative Integer —
  all enforced with clear errors. `_str_`, per CLAUDE.md, "returns a String", but the code does not enforce it: a
  non-String return is just recursively stringified (`o.str(ctx)`), so `_str_` returning a List/Integer silently
  produces that value's repr instead of erroring. Minor leniency but an inconsistency in the dunder contract.
- failure-scenario: `_str_` returning `[1,2]` yields `"[1, 2]"` from `String(inst)` rather than a clear type error.
- proposed-test: assert `String(inst)` where `_str_` returns a non-String either errors or matches documented behaviour.
- proposed-fix: decide the contract (enforce String, or document the leniency) and add a test either way.
- confidence: high (behaviour is clear from the code)

### A07-6: `hash()`/`equals()`/`truthy()` invoke user code that may allocate/mutate during a Dict/Set operation
- severity: low
- location: `src/kirito/runtime.hpp` `InstanceValue::hash` (~1742), `equals` (~1769), `truthy` (~1724)
- category: weak-spot
- description: These `const` slots dispatch to Kirito methods via `activeVM()`. A `_hash_`/`_eq_` that mutates the
  instance or allocates (triggering GC) runs *inside* a fum container's probe. `class_hash.ki` already covers a
  mutating `_hash_` and a self-referential attribute without crashing, and `ProbeScope guard(s.probing_)` (runtime.hpp
  ~854/877) defends the bucket against reentrant rehash — good. Residual concern: a `_hash_`/`_eq_` that *inserts the
  same instance into the very container being probed* (reentrant mutation of the live container, not just any Dict) is
  the classic footgun; `class_hash.ki` deliberately avoids exercising it ("would recurse", line ~388). Whether
  `ProbeScope` fully covers self-insertion into the in-flight container is worth a targeted test.
- failure-scenario: `_hash_` that does `someLiveSet.add(self)` where `someLiveSet` is mid-insert.
- proposed-test: adversarial — `_hash_`/`_eq_` mutating the exact container being inserted into; assert no crash / clean error.
- proposed-fix: none if `ProbeScope` already covers it; otherwise bound/reject reentrant mutation of the probed container.
- confidence: low (partially mitigated; needs a test to confirm the boundary)

### A07-7: Bound methods are fresh allocations each `getAttr`; identity/equality of `obj.m` is never stable
- severity: low
- location: `src/kirito/runtime.hpp` `makeBoundMethod` (~1662), `InstanceValue::getAttr` (~1696)
- category: weak-spot / semantic note
- description: Every `obj.method` access allocates a new `NativeFunction` wrapper (receiver + method captured).
  Consequences: (a) `obj.m == obj.m` is `False` and `id(obj.m) != id(obj.m)` because `NativeFunction::equals` is
  identity — no bound-method value identity (Python has `==` on bound methods); (b) allocation churn if a bound method
  is fetched in a hot loop; (c) `inspect(obj.m)` shows `...` (the wrapper has no signature) rather than the underlying
  function's parameters. None are correctness bugs but are untested behaviours a user may rely on.
- failure-scenario: `var m1 = obj.f\nvar m2 = obj.f\nm1 == m2` → surprising `False`.
- proposed-test: document + assert the chosen identity semantics for bound methods.
- proposed-fix: optional — cache bound methods per (instance,name), or at least add a test pinning the behaviour.
- confidence: high (behaviour is clear)

---

## Coverage gaps

### A07-8: Instantiation error path "takes no arguments (no _init_ defined)" is untested
- severity: low
- location: `src/kirito/runtime.hpp` `ClassValue::callFull` (~1653-1655)
- category: coverage
- description: `C(1)` where `C` has no `_init_` throws `"C() takes no arguments (no _init_ defined)"`. No test in
  `test_class_ops.cpp` / `test_classes.cpp` / scripts asserts this message or the arity error. Also untested: passing
  keyword args to a no-`_init_` class (`C(x=1)`), and a `_init_` that returns a non-None value (silently ignored,
  unlike Python which raises — worth pinning as intentional).
- proposed-test: `CHECK_THROWS`/`throwsMsg` on `C(1)` and `C(x=1)` for a no-`_init_` class; assert `_init_` return ignored.
- confidence: high

### A07-9: Explicit `_ne_` method and reflected `_eq_`/`_ne_` on the RIGHT operand are undertested
- severity: low
- location: `src/kirito/runtime.hpp` `applyBinaryOp` Eq/Ne branch (~2135-2164)
- category: coverage
- description: `probe_dunders.ki` only tests default `_ne_` (derived from `not _eq_`). A class that defines `_ne_`
  DIRECTLY (so `!=` does not just negate `_eq_`) is not tested. The reflected path — LHS non-instance, RHS instance
  with `_eq_`/`_ne_` (`5 == c`, `5 != c`) — has code (lines ~2151-2160) but no obvious test asserting it dispatches to
  the RIGHT instance's dunder rather than falling to structural equality.
- proposed-test: class defining `_ne_` returning a value that is NOT `not _eq_`; and `5 == c` / `5 != c` reflected cases.
- confidence: medium

### A07-10: Native-value vs user-instance sharing a Dict/Set bucket (the `dynamic_cast` guard in `equals`) is untested
- severity: low
- location: `src/kirito/runtime.hpp` `InstanceValue::equals` (~1776), `applyBinaryOp` (~2138/2153)
- category: coverage
- description: `equals` uses `dynamic_cast<const InstanceValue*>` specifically because every `NativeClass`
  (DateTime/Matrix/Bytes/…) also reports `ValueKind::Instance`; a raw downcast would read a garbage handle (UB) when a
  user instance and a native value collide in a bucket. This is a deliberately-defended reachable-UB path but I found
  no test that actually places a user instance and a native object (with a colliding `_hash_`/hash) in the same Dict/Set.
- proposed-test: construct a Dict/Set holding both a user `_hash_` instance and a native value that hashes into the same
  bucket; assert lookups behave and there is no crash under asan.
- proposed-fix: none (code is correct); add the regression test.
- confidence: medium

### A07-11: `_len_` return-type/negative validation and `_iter_`-returns-non-iterable error paths lack tests
- severity: low
- location: `src/kirito/runtime.hpp` `InstanceValue::length` (~1846-1852), `iterate` (~1860-1867)
- category: coverage
- description: `_len_` must return a non-negative Integer (two distinct throws: non-Integer, negative) — neither error
  is asserted in the object-model tests. `_iter_` returning a non-iterable (e.g. an Integer) throws via the result's
  `iterate` — untested. `_call_`-with-kwargs on a NATIVE `_call_` ("does not accept keyword arguments", ~1836) untested.
- proposed-test: `throwsMsg` on `len(x)` with `_len_` returning `-1`/`"x"`; `for v in x` with `_iter_` returning `5`.
- confidence: high

### A07-12: `NativeParam` default/annotation binding via `NativeFunction::bindArgs` has no direct unit test in these files' suite
- severity: low
- location: `src/kirito/function.hpp:78-102` (`bindArgs`)
- category: coverage
- description: `bindArgs` enforces too-many-positional / unknown-keyword / duplicate / missing-required for signatured
  natives. It is exercised indirectly through stdlib kwarg tests, but the object-model test files don't pin the four
  distinct error messages, nor the "keyword out of positional order binds by name" path directly on a `NativeFunction`.
- proposed-test: a small embed-API test registering a signatured NativeFunction and driving each error/branch.
- confidence: medium

### A07-13: A class-body `var f = <existing function value>` binds as a method (receiver prepended) — undocumented surprise, untested
- severity: low
- location: `src/kirito/runtime.hpp` `InstanceValue::getAttr` (~1687-1696)
- category: coverage / semantic note
- description: The rule "only Function/NativeFunction class members bind as methods" means a class-body
  `var handler = some_module.fn` (intended as a stored callable, not a method) is returned as a bound method with the
  instance prepended as the first arg — likely surprising. `test_audit_fixes.cpp` tests that non-function members are
  NOT bound, but the inverse (a stored function value IS bound) is untested and worth pinning.
- proposed-test: class with `var f = <plain function>`; assert `inst.f()` prepends the receiver (documents the behaviour).
- confidence: medium

---

## DRY

### A07-14: Positional+named→slot argument binding is implemented three times
- severity: low
- location: `NativeFunction::bindArgs` (function.hpp:78-102), `KiFunction::callFull` (runtime.hpp ~2005-2040),
  `InstanceValue::callKw` (runtime.hpp ~1821-1837 forwards to callFull)
- category: DRY
- description: `bindArgs` and the slow-path of `callFull` share a near-identical algorithm (positional fill →
  match named by name → fill defaults → errors for too-many / unknown-keyword / duplicate / missing-required), with
  only cosmetic differences in the error text ("function ..." vs "name() ...") and in how defaults are produced
  (precomputed `Handle` vs runtime-evaluated default expr). A shared helper templated over the "produce default"
  step would remove the duplication and keep the four diagnostics in one place.
- proposed-fix: extract a common binder; keep the default-evaluation strategy as a callback.
- confidence: high

---

## Positively verified (no action)
- `children()` GC roots are complete for the audited types: `ClassValue` (methods + base), `InstanceValue`
  (cls + attrs; `selfHandle` is the object itself), `SuperValue` (instance + startClass), `KiFunction`
  (closure + ownerClass), `NativeFunction` (captures_ + defaults). Bound-method / `_super_`-builder lambdas
  pass their captured handles explicitly as `captures_`, so lambda-captured `Handle`s are rooted.
- `_bool_` contract (must return Bool; opt-in default always-truthy; attribute-not-method does not count;
  inheritance/override; throwing `_bool_` surfaces) is exhaustively covered by `spec_bool_dunder.ki`.
- `_hash_`/`_eq_` (compound/delegated/identity-only, `_eq_`-without-`_hash_` unhashable, return-type validation,
  throwing, boundary INT64, inheritance/override, broken-invariant no-crash, fuzz, post-construction attr cache) is
  exhaustively covered by `class_hash.ki`.
- `_super_` (parent ctor, method extension, multi-level one-per-call, baseless throws, missing member, overridable)
  covered by `test_super.cpp` + `probe_dunders.ki`.
- Shared class attribute + copy-on-write-on-assignment (read default, per-instance shadow on assign, mutable list
  shared by reference) covered by `r9_classes_proto.ki`, `r6_language.ki`, `test_audit_fixes.cpp`.
- Base-class-must-be-a-class check (`BuildClass`, bytecode_vm.hpp:171) prevents a bad downcast on a non-class base.
- The numeric/`==` reflected-symmetry path and the native-vs-instance `dynamic_cast` UB avoidance are correctly coded.
- `isPrivateName` correctly excludes single `_`, dunders (`_x_`), and public names; privacy is per class chain.
