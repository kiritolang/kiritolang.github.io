# A10 Serialization

**Status:** IN PROGRESS. Scanner A10, Kirito v1.15 audit — the NEW function/class value serialization.

Files: `src/kirito/stdlib_serde.hpp` (core flatten/rebuild), `stdlib_serialize.hpp` (KSER1 text),
`stdlib_dump.hpp` (KDMP binary). Cross-ref `locals.hpp` (freeVariables/eagerFreeVariables), parser
source-capture, `environment.hpp` (define barrier — confirmed barriered).

Baseline reading complete. `env.define` is barriered (environment.hpp:44). Pass-2 List/Set/Dict wires
use barriered `append/add/set`; Object attrs explicitly `gcWriteBarrier`. Depth guard 8000/1500 in
flatten's `visit`. Bytes/Matrix/DateTime/Random/Tensor are native `Instance`s serialized via
`_getstate_`/`_setstate_` (Stateful tag) + registered deserializer.

---
### A10-1: Deserializing a class/instance blob EXECUTES arbitrary embedded code at load (pickle-style RCE), undocumented  [severity: MED] [confidence: confirmed]
- **Location**: stdlib_serde.hpp:388-390 (`vm.evalIn(nd.s, S, "<deserialized class>")`) and :338 (function re-parse). Docs: CLAUDE.md `serialize`/`dump` bullets + docs/pages stdlib reference.
- **What**: `serde::rebuild` reconstructs a Class node by RE-RUNNING its verbatim `class ...:` source through `vm.evalIn`. A class body's non-method statements (eager class-variable initializers) run at class-build time — i.e. **at `loads()` time**. An attacker who controls a serialized blob therefore achieves arbitrary code execution the moment a victim calls `dump.loads` / `serialize.loads` / `.load()` on it. Confirmed: a class with `var marker = import("io").eprint("SIDE-EFFECT-AT-CLASS-BUILD")` fired that side effect during `serialize.load()` in a fresh VM. A malicious body could `import("sys").shell("...")`. The in-code comments call deserialize "a trust boundary" only for MEMORY-SAFETY bounds (checkId / eager-count clamp); the **code-execution** vector is nowhere warned about, and the docs present serialize/dump as ordinary data persistence.
- **Repro**: dump a class whose body has a side-effecting eager class-var initializer; `serialize.load()`/`dump.loads()` it in a fresh VM -> the initializer runs. (Verified end-to-end.)
- **Proposed fix**: Not a code bug per se — it is inherent to the source-reparse design (same class as Python `pickle`). The fix is a prominent SECURITY WARNING in the docs (`serialize`/`dump` pages, CLAUDE.md): "Never `loads`/`load` data from an untrusted source — deserializing a Function/Class blob re-parses and runs code." Optionally add an opt-in `allow_code=False` mode that refuses Function/Class/Stateful tags (data-only), for untrusted inputs.
- **Proposed test**: a doc-invariant note + (if opt-in added) a test that `loads` of a Class blob with `allow_code=False` throws.

### A10-2: Eager class-var initializer that calls a captured function referencing another user class fails to DESERIALIZE (serialize succeeds -> loads throws)  [severity: MED] [confidence: confirmed]
- **Location**: stdlib_serde.hpp:348-406 (pass 0c class rebuild) + :356-367 (`eagerDepsReady`) vs :554-565 (pass 5 free-var binding).
- **What**: A class's eager initializers run during pass 0c (`vm.evalIn(nd.s, ...)`). Any FUNCTION captured as an eager free variable has its shell built (pass 0b) but its OWN free variables are bound only in pass 5 (placeholders = None until then). `eagerDepsReady` only waits on eager deps that are directly `Tag::Class` nodes — it does NOT account for a user class reachable *transitively* through an eager function call. So if an eager class-var initializer calls a captured closure that instantiates another user class, that class reference is still a None placeholder at rebuild time.
- **Repro** (round-trips live, throws on load):
  ```
  # producer:
  class A:
      var _init_ = Function(self): self.tag = "A-built"
      var who = Function(self): return self.tag
  var makeA = Function(): return A()
  class B:
      var thing = makeA()            # eager: calls makeA() -> A() at class-build
      var get = Function(self): return self.thing.who()
  dump.save(B, "eager.kdmp")
  # consumer (fresh VM):  dump.load("eager.kdmp")
  #   -> "cannot deserialize class 'B': type 'None' is not callable"
  ```
  (Verified: producer runs clean; `dump.load` throws `type 'None' is not callable`.)
- **Proposed fix**: bind a function's captured free vars BEFORE any class whose eager initializer transitively needs them — e.g. treat a Function that is an eager free var of a class as needing its own (transitive) class deps ready, and bind function free vars eagerly (real values) rather than deferring all of them to pass 5. Minimal-risk alternative: in pass 0c, before running a class's eager initializer, eagerly bind the free vars of any *function* it captures eagerly (recursively) to their real values. Contract-preserving because a fully-built function value already exists in `objs[]` by pass 0c.
- **Proposed test**: the repro above as a `.ki` round-trip (dump+loads in one VM already reproduces it — no fresh VM needed since the failure is in rebuild, not registration).

**A10-2 UPDATE (broader + same-VM masks it):** the defect is NOT limited to captured user CLASSES. It
fires for ANY free variable of the eager-called helper that is deferred to pass 5 — including a captured
**module**. Repro: `var compute = Function(): return math.sqrt(16.0)` with `class Cfg: var root =
compute()`. In a **fresh VM** (the headline "loads into a fresh VM" scenario) `dump.load` throws
`cannot deserialize class 'Cfg': type 'None' has no attribute 'sqrt'` (compute's `math` free var is a
None placeholder at pass 0c). CRITICAL SECONDARY ISSUE: the **same-VM** round-trip
`dump.loads(dump.dumps(Cfg))` **succeeds** (prints 4.0) while the **fresh-VM** `dump.save`+`dump.load`
**fails** — a non-deterministic same-VM-vs-fresh-VM divergence. This means an in-process test
(`test_vm_serialization.cpp` uses fresh VMs, but a `.ki` round-trip test in the same VM would NOT) can
pass while real cross-process/parallel transfer fails. Any regression test for this MUST use a fresh VM
(or `parallel`), not a same-VM round-trip. This raises A10-2 toward HIGH for the module-helper variant
since eager class-vars computed by a stdlib-using helper are a common config pattern.
