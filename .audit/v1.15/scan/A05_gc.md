# A05 Generational GC

Status: COMPLETE. Read: arena.hpp, object.hpp, vm.hpp (GC), pool.hpp, environment.hpp, handle.hpp,
runtime.hpp (barrier defs + all container/env/instance/module/class mutators + type methods),
collections.hpp, module.hpp, class_value.hpp, function.hpp, bytecode_vm.hpp, compiler.hpp,
stdlib_serde.hpp (rebuild), stdlib_tensor.hpp (autograd), stdlib_net.hpp (Response/Session),
stdlib_regex.hpp (Match), dispatcher.hpp (worker VM construction). Cross-checked EVERY children()
override against its post-construction store sites, both directions.

## Barrier-completeness cross-check (the correctness cliff)

The soundness contract: after a minor, `kGcOldAge==1` promotes every surviving young object, so no old
object can point at a young object EXCEPT via a store made *after* the last minor — and every such
store must route through a barriered mutator (or be into a still-young container). I verified the
barrier set is exactly the mutable subset of the trace set.

Trace set (every `children()` override) vs. its mutable stores:

| Type (children reports) | Post-construction Handle store | Barriered? |
|---|---|---|
| `EnvValue` (vars_, parent_) | define / assignLocal / setAt | YES (no-arena overload). parent_ immutable post-ctor. |
| `ListVal` (elems) | append / setElem / insertElem; setItem->setElem | YES (arena overload). reverse/sort/clear/pop only PERMUTE existing edges -> no barrier needed (correct). |
| `DictVal` (k,v buckets) | set (barriers value+key); setItem->set; update/setdefault->set | YES (arena overload). |
| `SetVal` (buckets) | add; every set-algebra op builds a FRESH set via add | YES (arena overload). |
| `InstanceValue` (cls, attrs) | setAttr; setItem->_setitem_->setAttr | YES (no-arena). cls set pre-alloc (young). |
| `ModuleValue` (members) | setAttr / setMember; ModuleBuilder uses setMember | YES (no-arena). frozen/.ki import writes into a pre-alloc unique_ptr (young). |
| `ClassValue` (methods, base, closure) | defineMethod | YES (no-arena). base/closure/def set pre-alloc in BuildClass (young); immutable after. |
| `SuperValue` (instance, startClass) | set in ctor only | immutable -> safe |
| `KiFunction` (closure_, ownerClass) | ownerClass set on fresh clone pre-alloc in BuildClass | immutable -> safe |
| `NativeFunction` (captures_, defaults) | ctor only (bound methods freshly built per getAttr) | immutable -> safe |
| `IterCursor` (items, source) | ctor/GetIter only; idx advances (not a handle) | immutable -> safe |
| `TensorVal` (node->parents) | set in makeAutograd* right after alloc, no arena alloc between | immutable -> safe; `grad` is C++-side `optional<FT>` (NOT a Handle) -> no GC concern |
| `MatchVal` (subject) | ctor only | immutable -> safe |
| `ResponseVal` / `SessionVal` (headersH, cookiesH) | ctor only; the Dicts' CONTENTS mutate via barriered DictVal::set | immutable field -> safe |
| BytesIO/File/DateTime/Random/Matrix/Complex/BigInt | NO children() override — hold no Handles | N/A |

Serde rebuild (stdlib_serde.hpp): List->append, Set->add, Dict->set (all arena-barriered); instance
attrs get an explicit `gcWriteBarrier(vm.arena(), &inst, av)` at line 489; classes rebuilt via
`vm.evalIn` (normal BuildClass path). Correct even under GC-on-every-alloc (containers promote
mid-rebuild but every wiring store is barriered).

Roots (`forEachRoot`) vs. every Handle-holding KiritoVM member: none_/true_/false_/undefined_/global_,
smallInts_, replScope_ (guarded), moduleCache_, pathCache_, arglist_ (guarded), classRegistry_,
tempRoots_, pinnedRoots_, bytecodeConsts_ (all Proto consts pinned in one place, compiler.hpp:794),
auxRoots_ (operand stacks). NOT roots but correctly so: protoCache_ (consts pinned separately),
lastTraceback_ (TraceFrame holds only strings), programByFile_/chunks_ (AST, not Objects). **Roots are
complete.**

sweepYoung in-place compaction: `young_[keep++]=slot` while range-iterating is correct (keep never
exceeds the read cursor; size fixed until the final resize). With kGcOldAge==1 the keep-branch is dead
code (every survivor promotes) so young_ always ends empty. Generation wraparound at UINT32_MAX retires
the slot off the free-list (ABA guard) — sound. resetRemembered wholesale-clear is sound *given*
kGcOldAge==1 (see A05-2). remembered_ raw Object* pointers never dangle: minors never free old
objects; majors clear remembered_.

**VERDICT: the barrier set is COMPLETE for the single-VM model (the shipped/tested configuration).**
The one genuine soundness gap is A05-1 (the no-arena barrier resolves its arena via activeVM(), which
is the wrong VM when 2+ VMs coexist on one OS thread — an embedder-only scenario, not reachable via the
one-thread-per-VM parallel model). Everything else checks out.

---

### A05-1: no-arena write barrier resolves the WRONG arena when multiple VMs live on one thread  [severity: MED] [confidence: likely]
- **Location**: src/kirito/runtime.hpp:69-74 (`gcWriteBarrier(Object*, Handle)`); reached from
  EnvValue::define/assignLocal/setAt (environment.hpp:44,51,68), InstanceValue::setAttr
  (class_value.hpp:134), ModuleValue::setAttr/setMember (module.hpp:37,43), ClassValue::defineMethod
  (class_value.hpp:79). activeVM lifecycle: vm.hpp:37 (push in ctor), runtime.hpp:3568 (pop in dtor),
  vm.hpp:389 (`activeVM()` = back of the thread-local stack = most-recently-CONSTRUCTED live VM).
- **What**: The no-arena barrier overload reaches the arena via `KiritoVM::activeVM()`, which returns
  the *most recently constructed still-live VM on this thread*, NOT necessarily the VM that owns
  `container`/`value`. `evalIn`/`run` do NOT re-push, so if a host constructs VM_A then VM_B on the
  same thread and later mutates an OLD container belonging to VM_A (e.g. `vmA.registerGlobal(name,
  vmA.makeInt(x))` after VM_A has GC'd its global to old), the barrier calls
  `vmB.arena().deref(vmA_handle)`:
  - if the slot index is out of VM_B's range -> spurious `"dangling handle"` throw from a plain
    registerGlobal/setAttr;
  - if the slot is in range but generations differ -> `"stale generation"` throw;
  - **if the slot is in range AND the generation collides** (very plausible for low slots — both
    arenas allocate none_/true_/false_ at slots 0,1,2 with generation 1) -> derefs a *different* object
    in VM_B, reads its `gcYoung()`, and may wrongly skip `remember()` on VM_A's container -> VM_A's old
    container is never enrolled -> VM_A's next minor frees the still-reachable young value -> **UAF in
    VM_A**.
  The arena overload (collections) is immune — it takes the correct `ObjectArena&` explicitly. The
  parallel dispatcher is immune — worker VMs are constructed on their own std::thread
  (dispatcher.hpp:693 inside runWorker), so each thread's `_activeStack` holds exactly one VM. The gap
  is purely embedder-facing (two+ VMs coexisting on one thread with the older one mutated after a GC).
- **Repro** (C++, two VMs on the main thread):
  ```cpp
  KiritoVM a;
  // churn `a` until a major promotes its global to old (or call a.collectGarbage() after allocating):
  for (int i = 0; i < 200000; ++i) a.makeInt(1000000 + i);
  a.collectGarbage();                 // a.global_ is now OLD
  KiritoVM b;                          // b is now activeVM() on this thread
  a.registerGlobal("x", a.makeInt(918273));  // barrier misroutes to b.arena(); a.global_ may miss remember()
  a.minorCollect();                    // frees the young 918273 if the barrier was skipped -> UAF
  // read back a's global "x" -> dangling handle / wrong value
  ```
  (Exact manifestation depends on slot/gen collision between the two arenas; the `none_/true_/false_`
  low slots collide by construction, so a global promoted into a low old slot is the reliable trigger.)
- **Proposed fix**: The clean fix is to give these mutators the owning arena rather than guessing it.
  Minimal, contract-preserving options: (a) add arena-taking overloads to EnvValue::define/setAt,
  InstanceValue::setAttr, ModuleValue::setMember, ClassValue::defineMethod and thread the arena from the
  call sites that have it (the VM/BytecodeVM paths already do); (b) short of that, document in CLAUDE.md
  / value.hpp that mutating an *older* VM's globals/attributes/members while a newer VM is alive on the
  same thread is unsupported, and add a debug assertion `activeVM()==owningVM` where determinable. A
  pure "conservatively always remember()" fallback is NOT possible here because `remember()` itself
  needs the correct arena.
- **Proposed test**: the C++ repro above, plus a positive control (same sequence with a SINGLE VM must
  keep the value alive). Also a guard test: construct A, run A to promote a global, construct B, mutate
  A, minor-collect A, assert the value survives.

### A05-2: resetRemembered wholesale-clear is silently coupled to kGcOldAge==1 (latent fragility)  [severity: LOW] [confidence: confirmed]
- **Location**: src/kirito/arena.hpp:58-64 (`resetRemembered`), object.hpp:123 (`kGcOldAge=1`),
  arena.hpp:123-139 (`sweepYoung` promote), vm.hpp:221 (minor calls resetRemembered).
- **What**: `minorCollect` clears the ENTIRE remembered set after every minor. This is sound ONLY
  because `kGcOldAge==1` guarantees `sweepYoung` promotes every surviving young object to old — so
  every recorded old->young edge became old->old and the old container legitimately has no young child
  left. If `kGcOldAge` were ever raised (e.g. to add a real aging/tenuring policy, a natural future
  tweak), a young object could survive a minor while STILL young; its old parent's remembered flag
  would be cleared, and the NEXT minor would free the still-reachable young child -> UAF. The coupling
  is documented in comments (arena.hpp:58-60, object.hpp:123) but nothing enforces it — the
  `young_[keep++]` branch in sweepYoung even pretends to support kGcOldAge>1 while resetRemembered
  quietly breaks it.
- **Repro**: COVERAGE GAP / would-be regression: change `kGcOldAge` to 2 and the "young referenced
  only by old survives repeated minors" test (test_gc_generational.cpp:107-121) would fail with a UAF
  on the SECOND minor.
- **Proposed fix**: add `static_assert(Object::kGcOldAge == 1, "resetRemembered's wholesale clear
  assumes promote-on-first-survival; a higher tenuring age needs a survivor-rescan of the remembered
  set");` next to `resetRemembered` (or in minorCollect). Zero runtime cost, makes the invariant a
  compile error to violate.
- **Proposed test**: N/A (the static_assert is the guard). Optionally document the coupling in
  CLAUDE.md's GC bullet ("the write-barrier remembered-set clear assumes promote-on-first-survival").

### A05-3: COVERAGE GAP — no test exercises the no-arena barrier arena-resolution or the multi-VM case  [severity: LOW] [confidence: confirmed]
- **Location**: tools/tests/unit/test_gc_generational.cpp (the barrier suite).
- **What**: The generational suite thoroughly covers List/Dict/Set (arena overload) and drives
  Instance/Module/EnvValue/Class barriers through *single-VM* Kirito execution (where activeVM() is
  trivially correct). No test constructs two VMs on one thread, and no test isolates the no-arena
  overload's arena resolution. So A05-1 is entirely unguarded, and any future refactor of the
  activeVM stack (or of the EnvValue/Instance/Module/Class mutators) could silently regress it.
- **Repro**: COVERAGE GAP.
- **Proposed fix**: N/A (add the tests in A05-1 / A05-2).
- **Proposed test**: the A05-1 two-VM C++ repro (negative + positive control) and the A05-2
  static_assert.

---

## Summary

Findings: 1 MED, 2 LOW (0 HIGH).

- **A05-1 [MED]**: no-arena write barrier (EnvValue/Instance/Module/Class mutators) resolves its arena
  via `activeVM()`, which is the wrong VM when 2+ VMs coexist on one OS thread -> possible UAF in the
  older VM. Embedder-only; NOT reachable via the one-thread-per-VM parallel model or any single-VM run.
- **A05-2 [LOW]**: `resetRemembered`'s wholesale clear is silently coupled to `kGcOldAge==1`; a future
  tenuring-age bump would make it unsound. Add a `static_assert`.
- **A05-3 [LOW]**: coverage gap — the multi-VM / no-arena-barrier arena-resolution path is untested.

**Barrier-completeness verdict: COMPLETE for the single-VM configuration (the shipped, tested model).**
Every type that reports a Handle in `children()` either (a) mutates it exclusively through a barriered
mutator (EnvValue/List/Dict/Set/Instance/Module/Class — verified both directions), or (b) sets it once
at construction while the container is still young and never rebinds it (Super/KiFunction/NativeFunction/
IterCursor/Tensor node-parents/Match/Response/Session). Reorder/remove ops that only permute existing
edges correctly skip the barrier. Roots are complete; sweepYoung compaction, generation-wraparound
retirement, and the remembered-set lifecycle are sound. Tensor `.grad` and all live-resource natives
(BytesIO/File/DateTime/Random/Matrix/Complex/BigInt) hold no traceable Handles. The single, narrow hole
is the cross-VM arena mis-resolution of the no-arena barrier overload (A05-1).
