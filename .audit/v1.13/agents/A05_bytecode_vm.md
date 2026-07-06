# A05 — Bytecode VM Execution Engine + Control/Exceptions

Audit of `src/kirito/bytecode_vm.hpp`, `src/kirito/control.hpp`, `src/kirito/exceptions.hpp`
(with cross-checks into `runtime.hpp`, `vm.hpp`, `compiler.hpp`, `object.hpp`, `collections.hpp`,
`builtins.hpp`, `stdlib_io.hpp`).

Status: COMPLETE.

## VM surface enumerated
- **Operand stack** (`bytecode_vm.hpp:52-462`): one `std::vector<Handle> stack_`; slots
  0/1/2 = scope/result/owner, locals at `[3 .. 3+localCount)`, operands above. Registered as a GC
  aux-root region (`pushAuxRoots(&stack_)`); frame is non-movable so the registered pointer never
  dangles. One frame == one `BytecodeVM`, recursing on the native C++ stack for nested calls.
- **Opcodes**: Load/Store/Assign Name/Local, Const/None/Result, Pop/Dup, Unary/Binary,
  Jump/PopJumpIf*/JumpIf*OrPop, Call/MakeFunction/BuildClass, Get/Set Attr/Item, GetSlice,
  Build List/Pack/Set/Dict/String, FormatValue, GetIter/ForIter, Unpack, SwitchMatch/SwitchDispatch,
  SetupBlock/PopBlock/Reraise/ExcMatch/Throw/Return. All operator/call/member semantics delegate to
  `runtime.hpp` free functions (`applyBinaryOp`/`applyUnaryOp`/`applyCall`/`evalMemberGet`/
  `checkPrivateAccess`/`isInstanceOf`/`spreadValues`/`scalarSwitchKey`) — **no re-implementation** (DRY
  is clean; see A05-8).
- **Iteration** (`IterCursor` obj, `GetIter`/`ForIter`): lazy (stream `LazyIterator`) preferred, eager
  `iterate()` fallback. Cursor lives on the operand stack (GC-traced via `children()`).
- **Exceptions**: runtime block stack `blocks_` (target ip + operand height), `unwind()` routes to the
  innermost block; try/catch/finally/with lowered by the compiler into SetupBlock/PopBlock + inline
  finally/exit duplication + frame-cleanup on break/continue/return. Three catch arms in `run()`:
  `KiritoError` (promoted to String), `KiritoThrow` (user value), `std::exception` (any native →
  String). Traceback appended per frame (`appendFrame`) innermost-first.
- **Guards**: call-count + native-stack-byte guard in `vm.hpp:enterCall` via `CallGuard` in
  `KiFunction::callFull`.

---

### A05-1: GC-root gap in `GetIter` eager branch — iterated items unrooted across `alloc(cursor)` (use-after-free)
- **severity**: high
- **location**: `src/kirito/bytecode_vm.hpp:293-313` (`Op::GetIter`, eager `else` branch, esp. the
  `alloc` at line 310); root cause interacts with `runtime.hpp:1024 StrVal::iterate`,
  `bytes.hpp:93`, `runtime.hpp:1860 InstanceValue::iterate`.
- **category**: bug / GC-root gap / memory safety
- **description**: In the eager branch, freshly-materialised iterator items are rooted by a
  `RootScope rs(vm_)` **declared inside the `else` block** (line 306). That `rs` is destroyed at the
  `else` block's closing brace (line 309), *before* `Handle r = vm_.alloc(std::move(cursor));`
  (line 310). `KiritoVM::alloc` runs `collectGarbage()` **before** inserting the new object
  (`vm.hpp:58-60`), so during that collection the not-yet-inserted `cursor` is unreachable and the
  items it holds in `cursor->items` are rooted by nothing. The still-rooted `iterable` (peek(0), not
  popped until after alloc) does **not** protect them when its `iterate()` returns *freshly-allocated*
  handles that are not its own `children()`:
  - `StrVal::iterate` allocates a fresh 1-char String per code point (`runtime.hpp:1024-1040`), and
    `StrVal::children()` is empty.
  - `Bytes::iterate` allocates fresh Integer bytes (`bytes.hpp:93`).
  - `InstanceValue::iterate` (`runtime.hpp:1860`) invokes user `_iter_`; the returned intermediate
    iterable is dropped, leaving its elements reachable only via the (now-swept) items vector.
  The comment on line 306 ("root ... until the cursor (a GC root) holds them") is wrong: the cursor is
  not a GC root until it is alloc'd into the arena *and* referenced by the operand stack — which
  happens only at `push(r)` on line 312, after the vulnerable `alloc`.
- **failure-scenario**: `for c in "hello":` (or `for b in someBytes:` / a class whose `_iter_`
  returns fresh values) with GC able to fire on `alloc(cursor)` — deterministically under
  `setGcThreshold(1)`, and reachable in long programs under the adaptive default. The char/byte
  handles are swept; the arena slots are freed and generations bumped; `ForIter`'s
  `push(cur.items[cur.idx++])` then pushes a stale handle → dead-handle deref (throw or
  garbage/UB) inside the loop body. An ASan GC-stress run would flag it.
- **proposed-test**: `KiritoVM vm; vm.setGcThreshold(1); vm.runSource("var s = \"\"\nfor c in
  \"abc\":\n    s = s + c\n"); CHECK(... == "abc")` — plus the same for a `Bytes` and a class with
  `_iter_` returning a freshly-built List. Run under ASan.
- **proposed-fix**: hoist the `RootScope` to enclose the `alloc(cursor)` (declare it before the
  `if (lazy)` and root the items in the eager branch so it stays alive through line 310), or root the
  items in `tempRoots_` with a `RootScope` at case scope. Simplest: move `Handle r =
  vm_.alloc(std::move(cursor)); pop(); push(r);` *inside* the `else`/`if` while `rs` is alive, or add a
  case-scoped `RootScope`.
- **confidence**: high

### A05-2: `excSpan_` clobbered by a nested try/catch inside a `finally` → reraised exception carries the wrong source span
- **severity**: low
- **location**: `src/kirito/bytecode_vm.hpp:362` (`Op::Reraise` reads `excSpan_`), `432-441`
  (`unwind()` writes `excSpan_`), `459` (`excSpan_` member); compiler `emitFinallyExc`
  (`compiler.hpp:428-433`) parks only the exception *value*, not its span.
- **category**: bug / diagnostic correctness
- **description**: `excSpan_` is a single per-frame slot holding "the span of the exception currently
  being unwound", set by every `unwind()` and read by `Reraise`. On a reraise path the compiler parks
  the in-flight exception *value* in a hidden local across the finally body (`$excN`) but does **not**
  save/restore `excSpan_`. If the finally body contains a nested `try`/`catch` (in the same function
  body) that itself unwinds, that nested `unwind()` overwrites `excSpan_` with the inner exception's
  span. When the outer `Reraise` fires (`throw KiritoThrow{v, excSpan_}`) it attaches the **inner**
  throw's span to the **outer** value.
- **failure-scenario**:
  ```
  try:
      throw "OUTER"          # want this span reported
  catch Integer as e:        # does not match a String -> reraise path
      pass
  finally:
      try:
          throw "INNER"      # its span leaks into excSpan_
      catch:
          pass
  ```
  The uncaught `"OUTER"` is reported/traced at the `throw "INNER"` line:col instead of its own.
- **proposed-test**: golden `.experr`/`.ki` asserting the reported line:col of the escaping OUTER
  exception is the `throw "OUTER"` line, with a caught nested throw in the finally.
- **proposed-fix**: park `excSpan_` alongside the value in `emitFinallyExc` (stash+restore a hidden
  span local, or carry the span in the parked value), or make `Reraise` take the span from the parked
  exception rather than the shared member.
- **confidence**: medium

### A05-3: No GC-stress test iterates a String/Bytes/user-`_iter_` object (would expose A05-1)
- **severity**: medium
- **location**: coverage — `tools/tests/unit/test_gc_stress.cpp` (uses `setGcThreshold(1/50/100/256)`
  but only over arithmetic/list-cycle workloads, never a `for x in <fresh-item iterable>`).
- **category**: coverage gap
- **description**: `test_gc_stress.cpp` exercises RootScope, pins, cycles, deep structures, and the
  pool, but never drives `GetIter`/`ForIter` over an iterable whose `iterate()` returns
  freshly-allocated handles under an aggressive threshold — exactly the path with the A05-1 root gap.
  The existing golden iteration scripts run with GC effectively off (large default threshold), so the
  bug is silent.
- **proposed-test**: the A05-1 test, added to `test_gc_stress.cpp`.
- **confidence**: high

### A05-4: A native `std::exception` crossing multiple VM frames loses its lower-frame traceback
- **severity**: low
- **location**: `src/kirito/bytecode_vm.hpp:390-399` (the `catch (const std::exception&)` arm builds a
  fresh `std::vector<TraceFrame> tb` each frame).
- **category**: bug / traceback fidelity
- **description**: The `KiritoError`/`KiritoThrow` arms accumulate frames onto the exception's own
  `.traceback` as it unwinds through nested `run()` frames, so an uncaught error carries the whole
  chain. The bare-`std::exception` arm cannot attach to the exception object, so each frame it crosses
  starts a **new** local `tb` with only that frame; the final catching frame's `setLastTraceback(tb)`
  therefore records just one frame, dropping the inner frames the native exception actually passed
  through. (Consistent with the A04-2 note, but only fixes the "stale previous chain" problem, not the
  truncation.)
- **failure-scenario**: a native module throws a `std::runtime_error` two Kirito call levels deep;
  `sys.traceback()` in an outer `catch` shows only the outer frame, not the call chain.
- **proposed-test**: a native function that throws `std::exception`, called through two nested Kirito
  functions inside a `try`; assert `sys.traceback()` lists all frames.
- **proposed-fix**: promote the native `std::exception` to a `KiritoThrow`/`KiritoError` at the
  innermost frame (converting once, then letting the accumulating arms carry it), instead of
  re-throwing the raw `std::exception` per frame.
- **confidence**: medium

### A05-5: `catch <non-type>` (e.g. `catch 5 as e:`) silently never matches instead of diagnosing
- **severity**: low
- **location**: `src/kirito/bytecode_vm.hpp:363-366` (`Op::ExcMatch`) → `runtime.hpp:2059
  isInstanceOf` / `resolveTypeName`.
- **category**: bug / missing diagnostic (weak spot)
- **description**: `ExcMatch` calls `isInstanceOf(vm, exc, type)`. When the catch-type expression is
  not a class, a built-in type constructor, or a type-name String, `resolveTypeName` returns `""` and
  `isInstanceOf` returns `false`. A nonsensical `catch 5 as e:` or `catch someInstance as e:` thus
  never matches and is silently skipped rather than raising "catch type must be a class or type".
- **failure-scenario**: `try: throw "x"\ncatch 5 as e: io.print("caught")` — the handler is silently
  dead; the exception propagates as if the clause were absent, confusing the author.
- **proposed-test**: `.experr` requiring a clear diagnostic for a non-type catch clause (or a golden
  documenting the intended silent-skip behaviour if that is deliberate).
- **proposed-fix**: in `ExcMatch` (or the compiler), reject a catch-type that resolves to neither a
  class nor a recognised type name with a `KiritoError`.
- **confidence**: medium

### A05-6: Reraise-through-nested-try-in-finally path untested (would expose A05-2)
- **severity**: low
- **location**: coverage — `tools/tests/scripts/spec_finally_control.ki`,
  `exceptions.ki`, `spec_exceptions.ki` cover break/continue/return-in-finally and propagation, but no
  case nests a self-contained `try/catch` inside a `finally` on a reraise path.
- **category**: coverage gap
- **description**: `spec_finally_control.ki` is thorough on operand-height correctness but does not
  exercise `excSpan_` interaction (A05-2). No test asserts the *reported location* of a reraised
  exception after a nested try/catch ran in the finally.
- **proposed-test**: the A05-2 test.
- **confidence**: high

### A05-7: `control.hpp` `Flow` enum is dead code (tree-walker remnant)
- **severity**: info
- **location**: `src/kirito/control.hpp:10` (`enum class Flow`), included by `kirito.hpp:9`.
- **category**: cleanliness / DRY
- **description**: `Flow` is unused anywhere in `src/` or `tools/` (grep for `Flow::` returns
  nothing). It documented the old tree-walking evaluator's statement-continuation signal; the bytecode
  VM uses jumps + the block stack instead. The header is still `#include`d by the umbrella but
  contributes only a dead enum.
- **proposed-fix**: delete `control.hpp` and its include, or keep only if intentionally reserved.
- **confidence**: high

### A05-8: (Positive) DRY is clean — VM owns no operator/call/member semantics
- **severity**: info
- **location**: `bytecode_vm.hpp` opcode handlers.
- **category**: DRY (no divergence found)
- **description**: Every semantic-bearing opcode delegates to the shared `runtime.hpp` free functions
  (`applyBinaryOp`/`applyUnaryOp`/`applyCall`/`evalMemberGet`/`checkPrivateAccess`/`isInstanceOf`/
  `spreadValues`/`scalarSwitchKey`) and the value protocol (`getItem`/`setItem`/`slice`/`iterate`/
  `lazyIterate`). `BuildList`/`BuildPack` are intentionally one implementation under two opcodes;
  `SwitchMatch`/`SwitchDispatch` both route through `scalarSwitchKey`. No re-implementation of shared
  semantics was found. Recorded so future changes preserve the boundary.
- **confidence**: high

---

## Areas checked and found sound (no finding)
- **Operand-stack GC rooting during `Call`**: args + the built `named` vector stay on `stack_`
  (an aux root) until `stack_.resize(base-1)` after `applyCall` returns. Safe.
- **`Unpack` slot rooting**: `spreadValues` roots internally; the return→push window contains no
  allocation (`pop()`/`push()` don't run Kirito GC). Safe (fragile-but-correct).
- **Lazy `ForIter`**: the cursor stays on the operand stack (rooted); `next()` allocates its item and
  returns it, `push(*step)` follows with no intervening allocation. Safe.
- **Slot-addressed locals vs `unwind()`**: locals occupy `[3, 3+localCount)`, reserved first and never
  popped; block `stackHeight` is always ≥ that, so `stack_.resize(b.stackHeight)` never destroys a
  slot. Safe.
- **finally / with-exit on return/break/continue**: compiler `unwindFramesAbove` emits PopBlock +
  inline cleanup per crossed Block frame, innermost-first; exception paths park the value at clean
  operand height (`$excN`). Return-value preservation across a with-`_exit_` call verified. Correct
  (well-covered by `spec_finally_control.ki`).
- **Catch order** (`KiritoError` before `KiritoThrow`) is correct given the inheritance
  (`exceptions.hpp:54`); mis-order would mis-unwind (documented, honoured at every site checked).
- **Traceback line** (`appendFrame`): `code[ip-1]` after `code[ip++]` correctly names the faulting /
  call-site instruction; bounds-guarded. Sound.
- **Native-frame recursion guard** (stack-byte probe in `enterCall`) is tested via
  `sorted(key=g)`/`apply(h)`/`max(key=m)` self-recursion in `test_audit_v112.cpp:116-118`. Covered.
- **Depth guard** (`max_recursion.experr`), non-iterable for/unpack, uncaught throw, unpack
  count/star errors: covered by `tools/tests/errors/`.

## Summary
- Confirmed bugs: **2** (A05-1 high — GC use-after-free iterating String/Bytes/user-`_iter_`;
  A05-2 low — reraise span corruption via nested try in finally).
- Additional bugs/weak spots: **A05-4** (native-exception traceback truncation), **A05-5**
  (silent non-type catch).
- Coverage gaps: **A05-3**, **A05-6**. Cleanliness: **A05-7** (dead `Flow` enum). Positive: **A05-8**.
