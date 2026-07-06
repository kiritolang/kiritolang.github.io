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
