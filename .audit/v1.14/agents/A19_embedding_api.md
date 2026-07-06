# A19 — Embedding C++ API audit (v1.14)

Subsystem: value.hpp / native.hpp wrappers, RootScope, PinnedHandle, pin/unpin,
NativeClass/NativeModule/NativeFunction, ModuleBuilder, NativeParam signatures.
Focus: C++ TEST PARITY (this surface is unreachable from .ki).

STATUS: complete

## Scope reviewed
- `src/kirito/value.hpp` (1034 lines): Value base + Bool/Integer/Float/String/Bytes/List/Dict/Set
  wrappers, Args, PinnedHandle, detail::Pin/Anything, adopt() pinning, operator surface.
- `src/kirito/native.hpp` (208 lines): argString/argInt/requireArgs/sliceIndices, ModuleBuilder,
  NativeModule, NativeClass CRTP, makeMethod.
- Existing C++ tests (all registered in tools/tests/CMakeLists.txt and built):
  test_value, test_value_ops, test_value_containers, test_value_extra, test_cppref_deep,
  test_embedding_extra, test_pinned_handle, test_r4_cpp_api, test_r7_embed_api, test_r8_embed_api,
  test_protocol, test_serialize_native, test_r5/r6_internals, test_gc, test_gc_stress, test_arena.

## Overall assessment
The embedding API is EXCEPTIONALLY well covered from C++ — among the strongest test parity in the
repo. Every wrapper's construction, mirrored method surface, and operator surface has explicit
coverage; operator byte-parity vs Kirito source is checked (test_value_ops `same()`); PinnedHandle
has an exhaustive dedicated suite (copy/move/refcount/self-assign/vector-growth/real-world-churn);
makeMethod/bindArgs/NativeClass slots/registry isolation/ABA generation safety are all pinned.

The findings below are: ONE genuine latent GC-safety hazard (with a matching coverage gap) in the
Value method surface, plus a small set of low-severity coverage gaps. No high-severity correctness
bug found.

---

### A19-1: Value arithmetic/unary operator results are UNPINNED views over fresh allocations — GC-dangling
- severity: medium
- location: `value.hpp:983-1006` (operator+/-/*/ / % , floordiv, pow, unary-) — all wrap via the
  non-pinning `Value(*vm_, applyBinaryOp(...))` / `Value(*vm_, applyUnaryOp(...))` ctor.
  Backed by `runtime.hpp:155` numericBinary (returns `vm.makeFloat`/`makeInt` fresh, unrooted) and
  `vm.hpp:73-74` makeFloat/makeString (always `alloc`, never interned).
- category: memory (latent use-after-GC)
- description: The `Value(vm, Handle)` constructor deliberately does NOT pin (header comment
  value.hpp:64-69: "Non-allocating wrappers … leave pin_ null: the underlying object is already
  rooted somewhere the GC can see"). That premise HOLDS for wrapping an arg/element, but is FALSE
  for an operator result: `applyBinaryOp` returns a freshly-allocated, unrooted object for every
  Float result, every out-of-[-256,256] Integer result, and every String/List concat or repeat.
  The wrapping `Value` leaves `pin_` null, so the fresh result is protected by NOTHING once the
  expression returns. A subsequent arena allocation (which runs GC-before-alloc, vm.hpp:59) sweeps
  it. The generation guard turns the later read into a thrown "dangling handle" rather than silent
  corruption, but it is still a spurious failure where the embedder holds a valid-looking Value.
  Note the inconsistency: `String::operator+(const String&)` (value.hpp:477) takes the fast path
  and DOES pin (via the String fresh-alloc ctor's adopt()), so `String a + String b` is safe while
  the identical `Value a + Value b` on two Strings is NOT — same result, different lifetime safety.
- failure-scenario:
  ```cpp
  KiritoVM vm; vm.setGcThreshold(1);
  Value sum = Value(vm, 1.5) + Value(vm, 2.5);      // fresh Float 4.0, pin_ == null
  for (int i = 0; i < 100; ++i) vm.makeString("x"); // GC sweeps the orphaned 4.0
  double d = sum.asFloat();                          // throws (dangling) — expected 4.0
  ```
  Contrast `Float(vm, 4.0)` (adopt()-pinned) which survives identical churn.
- proposed-test: new block in test_value_ops.cpp under `setGcThreshold(1)`: compute each operator
  result into a Value, force many allocations, then read/stringify the result and assert it survived.
  Cover Float add (fresh), big-Integer add (INT64-range, fresh), String `+`, String `*`, List `+`,
  unary `-` on a Float. Currently every operator assertion stringifies immediately with no GC window.
- proposed-fix: make the operator overloads adopt (pin) their result, mirroring the fresh-alloc
  constructors — e.g. return a Value built through `adopt(*vm_, applyBinaryOp(...))` instead of the
  bare `Value(*vm_, h)`. (Pinning an interned small-int/Bool result is harmless, as PinnedHandle's
  interned-singleton test already shows.) Alternatively document that operator results must be
  immediately stored into a pinned wrapper/container — but that contradicts the ergonomic promise.
- confidence: high (mechanism verified end-to-end: makeFloat/makeString always alloc; applyBinaryOp
  returns the fresh handle unrooted; the Value ctor does not pin)

### A19-2: Same unpinned-view hazard on Value::call / getAttr / at, and List::pop
- severity: low-medium
- location: `value.hpp:281-297` (call/getAttr), `value.hpp:972-976` (at → getItem), `value.hpp:582-588`
  (List::pop)
- category: memory (latent use-after-GC)
- description: These four also wrap results in the non-pinning `Value(vm, Handle)`. They can return a
  handle that is NOT rooted after the call:
  * `getAttr` on a NativeClass commonly returns a FRESH `makeMethod` NativeFunction (unrooted) or a
    freshly-boxed scalar (e.g. Vec3/Grid getItem return `vm.makeInt(...)`), so `Value m =
    inst.getAttr("scale")` / `Value x = obj.at(0)` orphan a fresh object.
  * `call` returns whatever the callee produced — frequently a fresh allocation.
  * `List::pop()` removes the element from the backing vector THEN returns it — the list no longer
    roots it, so if the list was its only referent the popped Value dangles on the next GC.
  Wrapping an in-place container element (`List::operator[]`, `Dict::operator[]`, `Dict::get`) is
  SAFE because the element stays rooted by the still-live container — that distinction is the crux.
- failure-scenario: `Value v = xs.pop(); /* churn allocations */ v.asInt();` where `xs` held the only
  reference → `v` swept, read throws. Likewise `Value r = fn.call({...}); /* churn */ r.use();`.
- proposed-test: extend the A19-1 GC block: pop the sole reference then churn+read; call a fn that
  returns a fresh String then churn+read; getAttr a native method then churn+invoke.
- proposed-fix: same as A19-1 — adopt()/pin the result in these methods (or, for pop, pin before the
  pop_back). If deemed out of scope, document that call/getAttr/at/pop results are transient views.
- confidence: high (mechanism identical to A19-1; pop's orphaning is direct in the code)

### A19-3: No C++ test forces a GC between a Value operator/call/pop result and its use
- severity: coverage-gap
- location: test_value_ops.cpp (operators immediately stringified, default threshold),
  test_value_containers.cpp (pop tested without post-pop churn)
- category: coverage
- description: The GC-pinning stress tests exercise fresh-alloc CONSTRUCTORS under threshold(1)
  (List/Dict/Set built element-by-element) and items() survival, but NOT the operator/call/getAttr/
  pop result paths — the exact surface A19-1/A19-2 identify as unpinned. A regression that dropped
  pinning from a fresh-alloc constructor WOULD be caught; the operators dangling would NOT.
- proposed-test: the block described in A19-1/A19-2 (single file, `setGcThreshold(1)`, one CHECK per
  operator + call + getAttr + at + pop). This is the single most valuable missing C++ test.
- confidence: high

### A19-4: ModuleBuilder::alias error path (unregistered target) untested
- severity: coverage-gap
- location: `native.hpp:124-128` (throws `"alias target '<x>' not registered"`); only the SUCCESS
  path is tested (test_embedding_extra.cpp:62 `m.alias("twice2","twice")`).
- category: coverage
- description: `alias()` throws if the existing member isn't found. No test drives that throw. A
  refactor that silently created a dangling/empty alias would not be caught.
- proposed-test: in a NativeModule::setup, `CHECK_THROWS(m.alias("bad", "nonexistent"))` (or assert
  from Kirito that importing the module surfaces the error), plus confirm the message names the target.
- confidence: high

### A19-5: Set-algebra and Bytes operators via the Value facade not byte-parity tested from C++
- severity: coverage-gap
- location: value.hpp operator surface; test_value_ops.cpp `same()` covers Int/Float/String/List
  arithmetic but NOT `Set - Set`, `Set < Set`/`<=`/`>`/`>=` (subset/superset), nor Bytes `+`/`*`/`<`.
- category: coverage
- description: CLAUDE.md documents Set algebra via `-`/`<`/`<=`/`>`/`>=` and Bytes `+`/`*`/ordering,
  and the hunt brief explicitly calls for "Set algebra via operators" C++ parity. These route through
  applyBinaryOp so they should match Kirito, but no C++ test asserts `Value(setA) - Value(setB)`
  equals the Kirito `setA - setB`, nor the Bytes operators from the facade.
- proposed-test: add `same()`-style assertions for `Set(vm,{1,2,3}) - Set(vm,{2})`, the four
  subset/superset comparisons, and `Bytes + Bytes` / `Bytes * n` / `Bytes < Bytes`.
- confidence: medium (they very likely work; this is pure coverage)

### A19-6: makeMethod minArgs>0 branch and ModuleBuilder::moduleHandle not exercised from embedding-authored code
- severity: coverage-gap
- location: `native.hpp:168-204` makeMethod (the `i < minArgs` "missing required argument" throw at
  195-197 is only reached via built-ins like random.gauss / d.setdefault, not a custom NativeClass);
  `native.hpp:100` moduleHandle() (used by stdlib_io.hpp:537 for sibling capture, but the embedding
  pattern is not unit-tested as extension surface).
- category: coverage
- description: Every embedding test that calls makeMethod passes the default `minArgs=0`, so the
  required-leading-slot guard is only covered indirectly through internal stdlib methods. Likewise
  moduleHandle()'s "member captures the module to reach a sibling at call time" pattern — the
  documented reason it exists — is only validated behaviorally through io.stdout redirection, never
  as a small third-party-module unit.
- proposed-test: (a) a custom NativeClass method built with `makeMethod(..., minArgs=1)`, called
  keyword-only skipping the required leading slot → assert the "missing required argument" throw
  from the embedding side. (b) a tiny NativeModule whose fn captures `m.moduleHandle()` and reads a
  sibling member the user reassigned, proving the late-binding capture.
- confidence: medium (both are exercised somewhere internally; the embedding-facing contract is what's
  untested)

### A19-7: Value/PinnedHandle store a raw KiritoVM* — a wrapper outliving its VM is UB (no guard, no test)
- severity: non-bug (documented contract) / note
- location: `value.hpp:70-77` detail::Pin (`~Pin` calls `vm->unpinHandle`), `value.hpp:316` Value::vm_,
  `value.hpp:889/913` PinnedHandle (`~PinnedHandle`→reset→`vm_->unpinHandle`).
- category: memory (design contract)
- description: Every wrapper (and PinnedHandle, and detail::Pin) holds a bare `KiritoVM*`. If a
  pinned Value or a PinnedHandle outlives its VM, the destructor calls `unpinHandle` on freed memory
  (use-after-free). This is the inherent "VM outlives all handles/wrappers" contract and is correct
  as documented, but there is no guard and (correctly) no test attempts it. Worth an explicit note in
  the embedding docs' lifetime section so a host storing a PinnedHandle in an object whose lifetime
  isn't nested under the VM is warned. Not a fix target.
- confidence: high (design fact)

## Summary
- Findings: 1 medium (A19-1), 1 low-medium (A19-2), 4 coverage-gap (A19-3/4/5/6), 1 non-bug note (A19-7).
- No high-severity correctness bug. The embedding surface is otherwise the best-tested in the repo.

### Coverage-gap checklist (ranked, most valuable first)
- [ ] **A19-3 — GC survival of operator/call/getAttr/at/pop results** (setGcThreshold(1) + churn +
      read). Directly exercises the A19-1/A19-2 hazard; single new block in test_value_ops.cpp.
- [ ] **A19-1/A19-2 fix decision**: pin operator/call/getAttr/at/pop results (adopt), or document as
      transient. If fixed, A19-3 becomes the regression gate.
- [ ] A19-5 — Set-algebra (`-`, `<`, `<=`, `>`, `>=`) and Bytes (`+`, `*`, ordering) operators
      byte-parity vs Kirito, from the Value facade.
- [ ] A19-4 — ModuleBuilder::alias() throw on an unregistered target.
- [ ] A19-6 — makeMethod(minArgs>0) "missing required argument" from a custom NativeClass;
      ModuleBuilder::moduleHandle() sibling-capture as an embedding unit.
- [ ] A19-7 — doc note only: wrapper/PinnedHandle must not outlive its KiritoVM.

### Confirmed WELL-COVERED (no action)
- All wrapper constructions (primitive/init-list/vector/Handle), unsigned/float overloads.
- Mirrored method surface: List [] /set/push(all overloads)/pop/contains/clear/range-for; Dict
  []/at/get/tryGet/set(all overloads)/contains/has/remove/keys/values/pairs/range-for; Set
  add/contains/discard/items/range-for; String codepoint [] /size/utf8/contains/starts-ends/concat/
  ==literal; Bytes []/data/size/ctors.
- Operator byte-parity vs Kirito for Int/Float/String/List (+ wraparound, true-div, throw-on-zero,
  exact IEEE-754 ==, unordered-throw) — test_value_ops `same()`.
- peek-then-wrap: isX→asX zero-alloc, all wrong-type asX/tryX throw/nullopt, None handling, empty Value.
- items() GC survival (A07-4), fresh-alloc constructor GC pinning under threshold(1).
- PinnedHandle: full copy/move/refcount/self-assign/reset/vector-growth/real-world-churn suite.
- NativeFunction signatures: bindArgs (positional+kw out-of-order, defaults, unknown/dup/too-many/
  missing throws), inspect output, NativeFnKw variadics, argInt/argString throws.
- NativeClass: every protocol slot (str/truthy/equals/hash/getAttr/setAttr/getItem/setItem[multi-key]/
  slice/iterate/length/contains/call/unary/binary/children/inspectMembers), CRTP defaults,
  serde opt-in (_getstate_/_setstate_ both codecs), registerDeserializer overwrite, makeMethod
  multi-capture GC-rooting.
- Registry/VM isolation, arena ABA/generation safety, dispatcher lifecycle.
