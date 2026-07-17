# A19 — C++ embedding API + native-extension glue (v1.15.1)

Scope: `src/kirito/value.hpp`, `native.hpp`, `vm.hpp` public surface, `object.hpp`, `handle.hpp`,
`arena.hpp`, `src/kirito.hpp`, embedding/extending docs, and the test coverage for all of it
(`tools/tests/unit/test_r4_cpp_api.cpp`, `test_r8_embed_api.cpp`, `test_cppref_deep.cpp`,
`test_protocol.cpp`, `test_arena.cpp`, `embed_*`).

Rules: no builds. Probe `./build-debug/ki` (esp. `--gc-threshold 1`, values outside the small-int
intern range). C++ probes are written here as ready-to-paste code for the main agent.

Status: IN PROGRESS

---
## Findings

### A19.1-1: `_hash_`/`_eq_`/`_bool_` dispatch via `activeVM()` — misroutes with 2+ VMs on a thread (the A05-1 landmine, still armed)  [severity: MED] [confidence: likely — code-confirmed, C++ probe written below, cannot compile]
- Location: `src/kirito/runtime.hpp:1792` (`InstanceValue::truthy`), `:1810` (`hash`), `:1837` (`equals`);
  the unchecked cast is `src/kirito/class_value.hpp:150` / `:87`.
- What: v1.15 A05-1 **deleted** the arena-less write barrier precisely because resolving the arena via
  `KiritoVM::activeVM()` misroutes when two VMs coexist on one thread. The *same* resolution survives
  untouched in the three user-class dunder slots:

  ```cpp
  inline std::size_t InstanceValue::hash() const {
      KiritoVM* vm = KiritoVM::activeVM();          // <-- NOT "the VM that owns this instance"
      const Handle* m = findMethod(vm->arena(), "_hash_");
  ```

  `activeVM()` is the most recently **constructed** still-live VM on the thread — `_activeStack()`
  is pushed in `KiritoVM::KiritoVM()` and popped in `~KiritoVM()`; there is **no enter/leave scoping**,
  despite the ctor comment claiming "the pointer is scoped to whichever VM most recently *entered* on
  this thread" (vm.hpp:34-36). Construct VM `A`, then VM `B`; run `A`'s code; every `_hash_`/`_eq_`/
  `_bool_` on an `A` instance is dispatched with `B`.
- Why it is a defect: it breaks the headline architectural contract ("One `KiritoVM` = one fully-
  encapsulated process … No global/static mutable state, so multiple VMs coexist and never interfere"
  — CLAUDE.md). Concretely `findMethod` is an **unchecked** downcast:

  ```cpp
  // class_value.hpp:150
  return static_cast<const ClassValue&>(arena.deref(cls)).findMethod(arena, n);
  ```

  `cls` is a handle into **A**'s arena, `deref`'d against **B**'s. Two outcomes:
  1. generations differ -> `dangling handle (stale generation)` throw (the A05-1 symptom: a spurious
     error from correct embedder code);
  2. **generations collide -> silent type confusion (UB).** This is the likelier outcome, not the
     exotic one: every VM ctor allocates `none_/true_/false_/undefined_/global_` + 513 interned
     small ints in the *same order*, so two VMs' low slots are near-identical and generation is
     `kFirstGen` on both until a slot is reused. `static_cast<const ClassValue&>` on a `ListVal` then
     reads `methods`/`base`/`hasBase` out of unrelated bytes.
- Reachability: **embedder-only** — checked and ruled out for `parallel`: `dispatcher.hpp:709`
  constructs `KiritoVM worker;` *inside* the worker's own `std::thread` lambda (`:585`), so each
  thread's `_activeStack()` holds exactly its own VM. Not reachable from `ki`. But the embedding API
  explicitly sells coexisting VMs, and A05-1 was accepted as MED on exactly this reachability.
- Repro: C++ probe `P1` below (cannot compile this round — no builds). Predicted: `dangling handle
  (stale generation)`, or a wrong/garbage hash.
- Proposed fix: the instance must not have to guess its VM. Cheapest faithful fix, mirroring how
  A05-1 was resolved (take the owner from the caller rather than a thread-local): give `InstanceValue`
  an owning-VM back-pointer set at instantiation (`runtime.hpp:1689` already runs there and has the
  `vm`), and use it in all three slots, keeping `activeVM()` only as a null fallback. It costs one
  pointer per instance and no lookup. The deeper fix — threading the arena into the `hash`/`equals`/
  `truthy` protocol slots — is the "right" one but touches every `Object` subclass; `equals` already
  *receives* `const ObjectArena&` and ignores it in favour of `activeVM()` (runtime.hpp:1849), so
  `equals` at least can be fixed with no signature change at all.
  Risk: none to a documented contract — this restores one.
- Proposed test: `tools/tests/unit/test_gc.cpp` already has a "two VMs on one thread" test from A05-1;
  extend it (or add to `test_r8_embed_api.cpp`): construct VM A, construct VM B, then in A define a
  class with `_hash_`/`_eq_`/`_bool_` and use an instance as a Dict key / in an `if`. Must not throw
  and must return A's answer. Asserts the symptom: "a second VM breaks the first VM's `_hash_`".

