# A09 Classes/Functions/Exceptions

Status: IN PROGRESS -> COMPLETE

Scope: `src/kirito/class_value.hpp`, `src/kirito/function.hpp`, `src/kirito/exceptions.hpp`, and the
class/function/exception runtime in `src/kirito/runtime.hpp` (callFull, method dispatch, `_super_`,
param binding, dunder dispatch). Read `.audit/README.md`'s false-positives table first; none of its
entries are directly in this subsystem's scope, but the general "documented tradeoff, don't re-flag"
posture applies.

## Method

1. Read `class_value.hpp`, `function.hpp`, `exceptions.hpp`, and the relevant sections of
   `runtime.hpp` (`ClassValue::callFull`, `KiFunction::callFull`, `makeBoundMethod`,
   `InstanceValue::getAttr`/`SuperValue::getAttr`, `checkPrivateAccess`, `evalMemberGet`,
   `applyBinaryOp`'s Eq/Ne dispatch, `_hash_`/`_bool_`/`hash()`), plus the `BuildClass` opcode in
   `bytecode_vm.hpp` (method-clone-per-class / ownerClass assignment) and the `with` compilation in
   `compiler.hpp`.
2. Read existing coverage: `test_classes.cpp`, `test_functions.cpp`, `test_kwargs.cpp`,
   `test_super.cpp`, `test_exceptions.cpp`/`test_exceptions_cpp.cpp`/`test_exceptions_deep.cpp`,
   `test_with.cpp`, plus a grep sweep of `tools/tests/scripts/*.ki` for `_super_`, mutable defaults,
   annotation subclass-satisfaction, private-access-from-outside, class-var COW, and `_exit_`-throws
   scenarios.
3. Adversarially re-derive the scenarios CLAUDE.md and the task brief call out, and where static
   reading left doubt, compiled `build-debug/ki` (already present in the repo) and ran throwaway `.ki`
   probes to observe actual behavior rather than trusting the reasoning alone.

## Findings

No new bugs were found in this subsystem. Every adversarial scenario probed below already matches
documented/intended behavior and (mostly) has existing regression coverage. This subsystem has been
through five prior audit rounds (pre-1.12, v1.12, v1.13, v1.14, 1.12.1) with dedicated class/function/
exception agents each time, and the `_super_`-corruption and privacy-bypass bugs those rounds found
are fixed and now regression-tested (`test_super.cpp`, `test_kwargs.cpp`'s super+kwargs test, the
ownerClass-clone-per-class comment block in `bytecode_vm.hpp:224-247`). This round's incremental value
is confirmatory: the adversarial probes below (run against the live binary, not just read) specifically
target the failure modes that historically bit this subsystem, and all came back correct.

### A09-0: Confirmed correct — shared function as a method of two classes with different bases (ownerClass / `_super_` isolation)  [N/A — confirms fix] [high]
- **Location**: `src/kirito/bytecode_vm.hpp:224-247` (BuildClass owned-clone loop), `runtime.hpp:2295-2323` (`evalMemberGet`'s `_super_` builder)
- **What**: v1.12.1 (A?? in that round) fixed "one function used as a method of two classes" corrupting
  `ownerClass`. Verified the fix holds under a harder case than the existing test: the SAME function
  object assigned as an *overriding* method (calling `self._super_()`) in two subclasses with
  *different, unrelated* base classes, called interleaved (not just once each) to rule out any
  process-lifetime memoization:
  ```
  class BaseX: var greet = Function(self): return "X"
  class BaseY: var greet = Function(self): return "Y"
  var sharedGreet = Function(self): return self._super_().greet() + "!"
  class SubX(BaseX): var greet = sharedGreet
  class SubY(BaseY): var greet = sharedGreet
  SubX().greet()  # X!
  SubY().greet()  # Y!
  SubX().greet()  # X!  (repeat, interleaved)
  SubY().greet()  # Y!
  ```
  Output was `X! Y! X! Y!` — no corruption. Each class's `BuildClass` clones the function with its own
  `ownerClass`, so `_super_()`'s `startClass` resolves against the correct base every time.
- **Repro**: see above; also verified a simpler private-member variant (function shared as a method of
  two classes, each reading its own instance's `_secret`) — correctly isolated (`a-secret`/`b-secret`).
- **Proposed fix**: N/A (already correct).
- **Proposed test**: worth adding the interleaved-different-bases variant above to `test_super.cpp` —
  the existing test only exercises a linear 3-level hierarchy with each method defined directly in its
  class, not the "same function object, two unrelated hierarchies, interleaved calls" shape.

### A09-1: Confirmed correct — class-body var mutation aliases across instances; only rebind (`self.x = v`) copies  [N/A — by design, matches doc] [high]
- **Location**: `runtime.hpp:1744-1749` (`InstanceValue::getAttr`, shared-class-attribute fallthrough), `InstanceValue::setAttr` (`class_value.hpp:133-136`)
- **What**: CLAUDE.md: "Non-function class-body `var`s are shared class attributes: instances read
  them through the class and copy-on-write **on assignment**." Verified the flip side is exactly as
  worded — a *mutation through a method* (no `self.x = ...` rebind, e.g. `self.items.append(x)` on a
  shared List class attribute) is NOT copy-on-write and aliases across every instance, same as
  Python's classic mutable-class-attribute footgun:
  ```
  class Bag:
      var items = []
      var add = Function(self, x): self.items.append(x)
  var a = Bag(); var b = Bag()
  a.add(1)
  b.items  # ['1'] too — aliased, not per-instance
  ```
  Confirmed `['1']`/`['1']`. A separate probe confirmed the *assignment* path DOES copy-on-write
  correctly (`self.n = v` on one instance leaves a sibling instance's class-level `n` untouched).
- **Repro**: see above.
- **Proposed fix**: N/A — this is the documented semantic (COW is scoped to assignment, not mutation),
  and it is the same tradeoff every "shared mutable default" language makes. Flagging only because the
  brief explicitly asked to probe it as a "weak spot".
- **Proposed test**: no test currently pins the *aliasing* half of this (only the COW-on-assignment
  half is implicit in various scripts); a small regression test asserting the mutation aliases would
  make the documented behavior harder to accidentally "fix" into copying semantics later (cf. the
  `.audit/README.md` false-positive pattern of well-intentioned "fixes" breaking a pinned contract).

### A09-2: Confirmed correct — recursion guard and instantiation storm survive `KIRITO_GC_THRESHOLD=1`  [N/A] [medium]
- **Location**: `vm.hpp:503-508` (`CallGuard`), `runtime.hpp:2037` (`KiFunction::callFull`'s guard)
- **What**: Ran a plain-recursion probe (`rec(1000000)`, no tail-call elimination) and a 20,000-iteration
  loop that instantiates a `Dog(Animal)` subclass and calls a `_super_()`-dispatching method each
  iteration, both under `KIRITO_GC_THRESHOLD=1` (the most aggressive collection cadence). Recursion
  threw a clean catchable `maximum recursion depth exceeded` (not a native-stack crash); the
  instantiation/`_super_` loop completed cleanly with the expected final value (`R199991!`). No
  use-after-GC corruption of instance attributes, method dispatch, or `_super_` resolution under
  maximal collection pressure.
- **Repro**: see method probes 4 and 5 in the working notes (not preserved as files — throwaway
  scratch scripts); trivially reproducible from the snippets described above.
- **Proposed fix**: N/A.
- **Proposed test**: N/A — `test_classes.cpp` already has a GC-threshold=300 instance survival test;
  a `KIRITO_GC_THRESHOLD=1` + `_super_`-dispatch variant would be a marginal but cheap addition.

### A09-3: Confirmed correct — nested `with`/`finally` unwind order (LIFO), and `_exit_` masking a body exception  [N/A — already documented + tested] [medium]
- **Location**: `compiler.hpp:561-589` (`WithStmt` compilation), `tools/tests/scripts/labx_exceptions.ki:145-176`, `r5_language.ki:83-120`
- **What**: Verified nested `with Ctx("A"): with Ctx("B"): throw "boom"` wrapped in an outer
  `try/finally` unwinds in the correct LIFO order — `enterA, enterB, exitB, exitA, innerFinally,
  caught` — matching Python's context-manager stacking semantics. Confirmed via a live probe (not
  just reading the compiler's `CFrame`/`cleanup` unwind machinery). The "`_exit_` that throws
  overrides/masks a body exception" behavior (also asked to probe) is already documented behavior with
  existing dedicated tests (`labx_exceptions.ki:167`, `r5_language.ki:112`).
- **Repro**: see probe 6 above.
- **Proposed fix**: N/A.
- **Proposed test**: N/A — adequately covered already.

### A09-4: Weak spot (not a bug) — a `_hash_` that mutates state breaks Dict/Set self-lookup, silently  [documented risk, low severity] [medium]
- **Location**: `runtime.hpp:1799-1819` (`InstanceValue::hash()`)
- **What**: A class whose `_hash_` mutates the instance's own state each call (e.g. increments a
  counter and returns it) produces a different hash on every call. Inserting such an instance into a
  Dict/Set and then testing membership on the *same instance* can silently return `False` (bucket
  computed at insert time no longer matches the bucket computed at lookup time) rather than throwing
  or otherwise flagging the contract violation:
  ```
  class Bad:
      var _init_ = Function(self): self.n = 0
      var _hash_ = Function(self): self.n = self.n + 1; return self.n
      var _eq_ = Function(self, o): return True
  var b = Bad(); var d = {}
  d[b] = "x"
  b in d   # False — inserted under hash 1, looked up under hash 2
  ```
  This is the exact scenario CLAUDE.md's audit brief calls out ("a `_hash_` that mutates") — it is not
  a language bug (every hash-table-backed language has this same obligation on `__hash__`/`_hash_`
  implementers: the value must be stable while the object is a live key), but it is worth recording as
  a confirmed weak spot rather than assuming it's handled, since a stale/incorrect hash produces a
  silently-wrong answer (a missed membership test) rather than a loud error.
  - severity is LOW because it requires a user's `_hash_` to violate its own contract; there is no way
    for the interpreter to detect "does this hash change" without re-hashing all live keys, which no
    hash table does.
- **Repro**: see above.
- **Proposed fix**: N/A — not fixable at the language level without changing the hash-table contract
  itself (would need to cache the hash at insertion and never re-derive it, which is a bigger design
  change out of scope for a patch round; also arguably wrong since two Bad() with equal user-`_eq_`
  should collide, and there is no way to know which hash was "the real one").
- **Proposed test**: could add a regression test that PINS the "silently doesn't find itself" behavior
  (so a future change doesn't accidentally start throwing or start caching in a way that changes other
  semantics) — a documentation-of-intent test rather than a bug-fix test.

### A09-5: Coverage gap map (dunder × inherited/via-super, function feature × combination)

Enumerated below; most combinations ARE covered somewhere in `tools/tests/scripts/*.ki` (this repo's
test suite is unusually thorough for a project at this audit maturity), the gaps are the residual
untested cells found by cross-referencing `probe_dunders.ki`/`labx_dunders.ki`/`r7_types.ki` against
the full dunder list in CLAUDE.md:

- `_getitem_`/`_setitem_` with **variadic keys** (`m[i, j] = v`) accessed via `self._super_()[i, j]`
  (i.e. a subclass's `_getitem_` extending the base's multi-key `_getitem_` through super) — not
  found in any script; `_getitem_`/`_setitem_` super-extension is untested (only direct-define and
  plain-inherit are covered for those two).
- `_iter_` defined in a base and invoked via `self._super_()` from an overriding subclass `_iter_`
  (i.e. "extend the parent's iteration, don't replace it") — not found; every `_iter_` super test found
  covers `_str_`/`describe`-style methods, not `_iter_`.
- `_contains_` inherited-only (never overridden, always reached via base) combined with `in`/`not in`
  through multiple inheritance levels (3+) — the multi-level super tests (`test_super.cpp`) use
  `describe`/`speak`, not `_contains_`.
- A **native-class-as-base**: none exists (`class Foo(SomeNativeType):` is presumably rejected or
  unsupported since native classes aren't `ClassValue`s) — not explicitly tested as a rejection case;
  worth a quick check that `class C(Matrix):` (or any built-in constructor as a base) produces a clear
  "base class must be a class" error rather than a confusing downstream failure. (`BuildClass` at
  `bytecode_vm.hpp:206` does check `baseObj.kind() != ValueKind::Class`, which should catch a native
  object base as long as native classes don't report `ValueKind::Class` — spot check, not a bug.)
- Function feature `Function(a, b=a+1)` combined with a **type annotation on the earlier param** and
  the default value NOT satisfying that annotation transitively (e.g. `Function(a: Integer, b = a):`
  where a caller passes an Integer for `a` but the default assignment path still re-checks `b`'s own
  (absent) annotation) — trivially fine since `b` has no annotation here; a genuinely interesting
  untested case is `Function(a, b: Integer = a):` where `a` is passed a non-Integer — does the
  annotation-check loop correctly reject `b`'s value derived from a non-Integer `a`? Not found in the
  scripts; likely correct (the same `typeMatches` check applies uniformly per-parameter after default
  evaluation) but unverified by an explicit test.
- Keyword arguments to a `_call_`-dispatched instance (`obj(x, k=1)` invoking `_call_`) chained with
  `_super_`-based `_call_` extension (a subclass overriding `_call_` and calling
  `self._super_()._call_(...)` — note `_call_` isn't invoked by name through `_super_`, it would have
  to be `self._super_()(...)` since `_call_` fires on parenthesized-call syntax against the super
  proxy) — not found in scripts; `SuperValue` has no `call`/`callKw` override in `class_value.hpp`
  (only `getAttr`), meaning `self._super_()(...)` (calling the super proxy itself, expecting it to
  dispatch to the base's `_call_`) would fall through to `SuperValue`'s absence of a `call` slot →
  `Object`'s default (not-callable) error, NOT the base class's `_call_`. This is a **plausible small
  gap**: extending `_call_` via `_super_()(...)` doesn't work the way extending `speak()` via
  `_super_().speak()` does, because the former needs a call on the super proxy itself while the latter
  needs a `.name` lookup on it first. Confirmed by reading `class_value.hpp:160-173` (`SuperValue` only
  declares `getAttr`, no `call`/`binary`/etc. overrides) — `Object`'s base `call()` is presumably an
  error stub. This is a real, verifiable gap; see A09-6 below where it's promoted to a checked finding.

### A09-6: `self._super_()(...)` does not extend an overridden `_call_` (SuperValue has no `call` slot)  [low-medium] [medium]
- **Location**: `src/kirito/class_value.hpp:160-173` (`SuperValue` declares only `getAttr`, not `call`/`callKw`/`binary`/`unary`/`getItem`/etc.)
- **What**: Every other operator dunder (`_add_`, `_eq_`, `_getitem_`, ...) is reached via `_super_()`
  only through a **named method lookup** (`self._super_().speak()`), which `SuperValue::getAttr`
  handles fine. But `_call_` is special: a subclass extending `_call_` would naturally want
  `self._super_()(...)` (call the super-proxy value directly, letting it dispatch to the base's
  `_call_`) rather than `self._super_()._call_(...)` (which — since `_call_` is a private-looking but
  actually non-private name with trailing+leading underscore, i.e. NOT private per `isPrivateName`,
  since it has a trailing underscore — would actually be directly name-callable too, so this may be a
  non-issue in practice if users are expected to spell it `self._super_()._call_(instance-omitted-args)`
  explicitly. Verified empirically:
  ```
  var io = import("io")
  class Base:
      var _call_ = Function(self, x): return x + 1
  class Sub(Base):
      var _call_ = Function(self, x):
          return self._super_()(x) * 10     # attempt: call the super-proxy directly
  Sub()(5)
  ```
  Confirmed this throws (`SuperValue` / `<super>` object is not callable) rather than dispatching to
  `Base._call_`. The WORKING spelling is `self._super_()._call_(x)` (explicit named lookup), which DOES
  work (verified separately) since `_call_` is a plain method name reachable via `SuperValue::getAttr`
  like any other.
- **Repro**:
  ```
  class Base:
      var _call_ = Function(self, x): return x + 1
  class Sub(Base):
      var _call_ = Function(self, x): return self._super_()(x) * 10   # THROWS: super not callable
  Sub()(5)
  ```
  vs. the working form `self._super_()._call_(x) * 10` (succeeds, returns 60).
- **Proposed fix**: either (a) accept this as a documented quirk — parenthesized-call syntax on a
  super-proxy is simply not supported, only explicit `._call_(...)` — and add one line to CLAUDE.md's
  `_super_` bullet noting operator-dunders must be extended by their **explicit dunder name**, not
  their operator syntax, through `_super_`; or (b) give `SuperValue` a `call`/`callKw` override that
  forwards to `startClass`'s `_call_` the same way `getAttr` forwards named lookups (a few extra lines
  mirroring `InstanceValue::call`/`callKw`). (b) is the more consistent fix given operator symmetry is
  a stated design goal elsewhere (kwargs "uniformly across every callable" etc.), but is genuinely a
  minor ergonomic gap, not a correctness bug — the explicit-name workaround exists and works.
- **Proposed test**: a regression test in `test_super.cpp` covering `self._super_()(...)` for a class
  overriding `_call_`, either asserting the current throw (if (a) is chosen) or the successful dispatch
  (if (b) is implemented).

## Summary

Full read of `class_value.hpp`, `function.hpp`, `exceptions.hpp`, and the class/function/exception
runtime paths in `runtime.hpp` + the `BuildClass`/`with` compilation in `bytecode_vm.hpp`/`compiler.hpp`,
cross-checked against `test_classes.cpp`, `test_functions.cpp`, `test_kwargs.cpp`, `test_super.cpp`,
`test_exceptions*.cpp`, `test_with.cpp`, and the `.ki` golden-script suite. This subsystem has been
through five prior hardening rounds; no new correctness bugs were found. Six adversarial live probes
against `build-debug/ki` (shared-function-as-method-of-two-different-hierarchies + interleaved calls,
class-var mutation aliasing vs. assignment COW, deep recursion + instantiation storm under
`KIRITO_GC_THRESHOLD=1`, nested `with`/`finally` LIFO unwind + `_exit_`-masks-body-exception, a
mutating `_hash_`, and return-type-annotation subclass enforcement) all came back matching documented/
intended behavior — see A09-0 through A09-3 for the confirmations, kept as findings (not just "looked
fine") because each is a live-run verification of a specific historical failure mode, not a re-read.

One genuine (minor) gap found: **A09-6** — `self._super_()(...)` does not dispatch to a base class's
overridden `_call_` because `SuperValue` only implements `getAttr`, not `call`/`callKw`; the explicit
`self._super_()._call_(...)` spelling works fine as a workaround. Low-medium severity, not previously
flagged in any prior round's `agents/*.md` (searched `v1.12`, `v1.13`, `v1.14`, `1.12.1` — no hit on
"SuperValue" + "call").

One documented-weak-spot note (not a bug, no fix proposed): **A09-4**, a `_hash_` that mutates its own
instance state breaks Dict/Set self-lookup silently — inherent to any hash-contract violation, not
interpreter-fixable, but worth a pinning test per the brief's ask.

A09-1's class-var-mutation-aliasing behavior is exactly as documented (COW is assignment-scoped, not
mutation-scoped) — recorded as a confirmation, with a suggested pinning test so it isn't accidentally
"fixed" into full copy semantics by a future round the way several other documented tradeoffs
historically were (see `.audit/README.md`'s false-positive table for the pattern).

Coverage-gap enumeration (A09-5) lists several untested dunder × `_super_` combinations
(`_getitem_`/`_setitem_`/`_iter_`/`_contains_` extended via `self._super_().method()`, a native class
as a base, and an annotated-default-derived-from-annotated-earlier-param case) that are plausibly fine
by code-path symmetry but are not explicitly pinned by any current test.
