# A08 — classes / dunders / inheritance / super / privacy / scoping

Audit of Kirito v1.16.1. Findings appended incrementally.

### F08-1 [Med] isinstance / typed-catch / type-annotation match user classes by NAME, not identity — same-named classes from different definitions conflate
- runtime.hpp:2083 `typeMatches` — walks the instance's class chain comparing `c.name == typeName` (a string). A resolved class arg is reduced to its `name` first (`resolveTypeName`, 2142). So an instance of one class `Foo` satisfies `isinstance(x, Foo)` / `catch Foo` / `-> Foo:` for an UNRELATED class also named `Foo`.
- Trigger (CONFIRMED, build-debug): a module `foomod` defines `class Foo`, the script defines its own `class Foo`; `isinstance(foomod.make(), Foo)` → `True` though the objects are instances of two different, unrelated classes. Same collision hits typed `catch Foo as e` and enforced parameter/return annotations.
- Why it matters: a soundness hole in the type gate — a subtype/instance check can pass for a value that is not of the intended type. Realistic whenever two modules (or a module + script) share a class name.
- Design tension: name-based matching is what lets serde/dump reconnect a deserialized instance to a rebuilt same-named class. But identity-with-name-fallback (prefer handle identity, fall back to name only for cross-VM/rebuilt cases) would close the in-VM hole. Contrast: privacy's `classIsSubclassOf` (2288) correctly uses handle IDENTITY (`cur == superH`).
- Fix idea: for an in-VM `isinstance(x, ClassValue)` where the arg is a live class handle, walk by handle identity; keep name-matching only for the String-name form and cross-VM reconnection. At minimum document the collision.
- Test to add: two same-named unrelated classes; assert `isinstance` / typed catch distinguish them (or document that they don't).
- Verified-real: YES (repro above).

### F08-2 [High] Deserialized instance loses `_bool_` custom truthiness — `hasBoolDunder` not restored in serde::rebuild
- stdlib_serde.hpp:566-567 — rebuild restores `inst->hasHashDunder` and `inst->hasEqDunder` (mirroring `ClassValue::callFull`), but NOT `inst->hasBoolDunder`. The default is `false`, so `InstanceValue::truthy()` (runtime.hpp:1830-1831) short-circuits to "always truthy" — the class's `_bool_` is never consulted after a round-trip.
- Trigger (CONFIRMED, build-debug): `class Flag: _bool_ returns self.v`; `dump.loads(dump.dumps(Flag(False)))` is TRUTHY though `Flag(False)` is falsy. Repro output: original → FALSE, restored → TRUE (wrong). `restored.v` is correctly `False`, so only the cached dunder flag is lost.
- Scope: affects BOTH `dump` (binary) and `serialize` (text) — they share `serde::rebuild` — and therefore `parallel` (cross-VM values travel via the dump codec), so a `_bool_`-bearing instance sent to a worker or checkpointed silently changes truthiness. Silent semantic corruption (no error), which is worse than a throw.
- Fix idea: add `inst->hasBoolDunder = cv.findMethod(vm.arena(), "_bool_") != nullptr;` right after line 567. (Consider factoring the three-flag cache into a shared `InstanceValue::cacheDunderFlags(ClassValue&, arena)` helper so callFull and rebuild can never diverge again — they already diverged here.)
- Test to add: dump/serialize round-trip of a `_bool_` class asserting the restored instance's truthiness matches the original in a fresh VM; likewise a parallel `spawn` returning/consuming such an instance.
- Verified-real: YES (repro above).

### F08-3 [Low] `!=` not symmetric when LEFT is a dunder-less instance and RIGHT defines a STANDALONE `_ne_` (no `_eq_`)
- runtime.hpp:2345-2373 — the reflected-onto-right branch (2361) is guarded by `if (l.kind() != ValueKind::Instance)`, so when the LEFT is an instance it is skipped; the fallback `kiEquals` (1646) consults `_eq_` on either side but NEVER `_ne_`. So a right-hand standalone `_ne_` is dropped.
- Trigger (CONFIRMED, build-debug): `class Plain` (no dunders), `class HasNe` (only `_ne_`); `Plain(1) != HasNe(1)` → `True` (structural, `_ne_` ignored) but `HasNe(1) != Plain(1)` → calls `_ne_`. Violates the documented "only ==/!= are symmetric" invariant (tools/tests/scripts/r11_docinvariants.ki).
- Note: the analogous `==` case is fine — `kiEquals` checks `_eq_` on both operands. The gap is specifically standalone `_ne_` (a class defining `_ne_` without `_eq_`), an unusual but legal shape. Also observed: a matched `_ne_`/`_eq_` returns its RAW value uncoerced (`h != p` yielded the String "NE-CALLED"), consistent with `_neg_`/`_not_` but worth a doc note for comparison dunders.
- Fix idea: in `kiEquals` (or the Eq/Ne branch) also try a standalone `_ne_` on either operand for `!=` when neither defines `_eq_`; or document that `_ne_` should not be defined without `_eq_`.
- Test to add: symmetry of `!=` for a right-only-`_ne_` class vs a dunder-less instance.
- Verified-real: YES (repro above).

### F08-4 [Low] callFull and serde::rebuild duplicate the dunder-flag cache logic — divergence-prone (root cause of F08-2)
- runtime.hpp:1724-1726 (callFull) sets all three flags; stdlib_serde.hpp:566-567 (rebuild) sets only two. The two copies of "walk the chain, cache hasX" already drifted (F08-2). Only two InstanceValue construction sites exist (grep-confirmed), so a single `InstanceValue::cacheDunderFlags(const ClassValue&, const ObjectArena&)` helper called from both removes the whole class of bug.
- Not itself a runtime defect; recorded as the maintainability fix that prevents F08-2 from recurring.
- Test to add: covered by F08-2's round-trip test.
- Verified-real: N/A (design).

---

## Verified-correct (checked, no defect found — coverage confirmation)
All exercised on build-debug:
- super: multi-level `_super_()` climbs one level per call (`C->B->A`); operator dunder via super by NAME works (`self._super_()._add_(o)`); operator SYNTAX on the view throws uniformly — `()`→"is not callable", `[]`→"is not indexable", `+`→"does not support this binary operator", `in`→"does not support 'in'", `for`→"is not iterable"; `_super_()` on a baseless class throws; overriding `_super_` (class defines `_super_`) is honored (built-in builder skipped, runtime.hpp:2433).
- privacy: `_name` accessible from self/subclass methods, denied to an outside function; a closure defined inside a method inherits ownership (touches `self._private`, resolves `self._super_()`) — bytecode_vm.hpp:213; the shared-function/two-classes clone story is correct — the OWNED CLONE gets `ownerClass`, the original module-level fn stays unowned and is DENIED private access; `hasattr` is privacy-agnostic (True on a private); dunder `_x_` stays public; per-chain identity via `classIsSubclassOf` (handle identity, not name).
- dunders: `_init_` defaults are per-call fresh (mutable default), reference earlier params (`m = n + 1`), forward keyword args; `_bool_` must return Bool (checked); `_hash_`/`_eq_` invariant holds in Dict/Set (equal keys collide); `_eq_`-only class is unhashable; `_getitem_`/`_setitem_` variadic keys (`g[1,2]=v`); `_iter_`/`_next_` generator protocol + StopIteration end + PEP-479 (deeper StopIteration leak propagates); `_len_` does NOT confer truthiness (no `_bool_` → always truthy).
- inheritance/annotations: subclass satisfies a base type annotation (param + return), return-annotation violation throws; class-var COW (assignment copies, shared-list mutation is visible to all).
- scoping: read-before-write → `name 'X' is not defined` (runtime, unbound-slot); self-shadowing initializer (`var sorted = sorted(x)`, and `var peek = peek` in a class body) → error; method self-recursion + mutual recursion by bare name resolve via class-scope rebinding to owned clones; sibling-method free-var reference reaches the owned clone (privacy preserved).

