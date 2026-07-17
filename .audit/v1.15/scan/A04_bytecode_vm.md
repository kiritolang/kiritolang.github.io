# A04 Bytecode VM

Status: COMPLETE

Scope: src/kirito/bytecode_vm.hpp, src/kirito/control.hpp; cross-referencing runtime.hpp and
compiler.hpp (SetupBlock/PopBlock/finally/with codegen) since the two co-design the block stack.

Method: static read + adversarial `.ki` snippets run against build-debug/ki (nested
try/finally/with/loops with return/break/continue/throw at every level, deep recursion,
higher-order natives, GC pressure).


### A04-1: `excSpan_` clobbered by a nested try/catch inside a `finally` — reraised exception carries the wrong source span  [severity: MED] [confidence: confirmed]
- **Location**: `src/kirito/bytecode_vm.hpp:501-510` (`unwind()` unconditionally overwrites `excSpan_`),
  `bytecode_vm.hpp:425` (`Op::Reraise` reads `excSpan_`), `bytecode_vm.hpp:528` (the single scalar
  member). Compiler side: `compiler.hpp` `emitFinallyExc` (parks the exception *value* in a hidden
  local across the finally body, but has no equivalent mechanism for the *span*).
- **What**: `excSpan_` is one per-frame scalar holding "the span of the exception currently being
  unwound", read by `Reraise` (`throw KiritoThrow{v, excSpan_}`) to give the re-thrown exception its
  original source location. It is written by every call to `unwind()`. When a `finally` body (running
  on the exception path, via `emitFinallyExc`) itself contains a nested `try`/`catch` that throws and
  is fully handled *inside* that finally, the nested exception's own `unwind()` overwrites `excSpan_`
  with its span. The exception *value* survives correctly (it is separately stashed/reloaded through a
  compiler-generated hidden local, `$excN`), but by the time the outer `Reraise` fires, `excSpan_` still
  holds the **inner**, already-resolved exception's span — so the reraised (outer) exception is reported
  at the wrong line:col. This is a recurring, previously-flagged, still-open finding: v1.13 A05-2,
  re-confirmed still open in v1.14 A04-3, apparently missed by the v1.14.1 A04 re-scan (which reported
  the engine "clean" with no mention of it) — reproduced again here against the current `build-debug/ki`
  binary (v1.15.0), so it has never actually been fixed.
- **Repro** (confirmed live against `build-debug/ki`):
  ```
  var f = Function():
      try:
          throw "original-error"     # line 3 — this is the exception that should be reported
      finally:
          try:
              throw "inner-handled"  # line 6 — fully caught right here, should have NO effect
          catch String as e2:
              discard e2

  f()
  ```
  Actual output:
  ```
  Traceback (most recent call last):
    File "t13.ki", line 10, in <module>
    File "t13.ki", line 2, in f
  t13.ki:6:13: error: uncaught exception: original-error
  ```
  The reported location is `6:13` (the inner, fully-handled throw) instead of `3` (the actual site of
  `original-error`, the exception that is genuinely propagating uncaught). Confirmed the exception
  *value* itself is untouched (verified with an outer `catch String as e: io.print(e)` → prints
  `original-error` correctly) — only the diagnostic span is wrong. Also reproduces identically with
  `catch` nested inside a `with`'s exit path, since `with`/`try` share the same `SetupBlock`/`unwind`
  machinery.
- **Proposed fix**: N/A (read-only scan) — but per the prior rounds' analysis, the fix belongs in the
  compiler's `emitFinallyExc`/return-parking machinery: stash the span alongside the parked exception
  value (e.g. encode it into the hidden local, or give `Op::Reraise` its span from a value carried on
  the stack rather than the shared `excSpan_` scalar), or make `excSpan_` push/pop symmetric with
  `blocks_` (tricky because the exception-path `SetupBlock`/`unwind` pairing has no matching "resolved,
  restore old span" bytecode point today — an explicit clear/restore op would need to be emitted
  wherever a handler fully resolves its exception, e.g. at `Lhandled`).
- **Proposed test**: pin the repro above as a C++ unit test in `test_exceptions.cpp` (or a golden
  `.ki` in `tools/tests/errors/`) asserting the reported line is `3`, not `6`. None of the existing
  tests (`test_exceptions.cpp`, `test_exceptions_deep.cpp`, `test_with.cpp`) cover a *nested, fully-
  resolved* try/catch running inside a `finally`/`with`-exit that is itself unwinding a different,
  still-in-flight exception — this exact interaction (this finding's `Op::Reraise`/`excSpan_` bug) is
  the reason it keeps slipping through re-scans.

## Coverage gaps (no bug found, but the combination is untested)

- **A04-G1**: No test exercises `break`/`continue` inside a `with` inside a `for` loop (manually
  verified correct in this round — see repro `t1.ki` below — but `test_with.cpp` has no loop-control
  coverage at all, and `test_exceptions.cpp`'s break/continue-in-try tests don't involve a `with`).
- **A04-G2**: No test exercises an exception thrown *from* a `finally` body itself (replacing the
  original in-flight exception) — manually verified correct (the newer exception wins, the original is
  discarded, no chaining) in this round (`t3.ki`/`t6.ki` below), matching the finally-throws-replaces
  design already implied elsewhere, but this exact case has no pinned regression test.
- **A04-G3**: No test exercises a `return` crossing three or more nested `with`/`finally` levels with a
  live return value (verified correct here — `t5.ki` below, returns `42` through `with A / with B /
  try-finally / with C`). Existing tests only cross one level.
- **A04-G4**: No test exercises `BuildClass` (a fresh `class` statement, including subclassing +
  `_super_` + sibling-method calls through the owned-clone rebind) executed repeatedly under
  `--gc-threshold 1` / `setGcThreshold(1)`. Manually verified correct and stable across ~200 iterations
  of class construction + instantiation + inherited/sibling method calls under GC-every-alloc (`t10.ki`,
  `t11.ki` below) — this is exactly the scenario the audit brief flagged as a barrier-correctness risk
  (`klass.defineMethod` / `classEnv.define` are both correctly barriered — confirmed by code read and by
  this stress test), but no C++/`.ki` regression test pins it, so a future regression in the barrier
  would go unnoticed until a flaky GC-timing-dependent corruption in the field.
- **A04-G5**: `control.hpp`'s `Flow` enum (`Normal`/`Break`/`Continue`/`Return`) has **zero** references
  anywhere in the codebase (`grep -rn "Flow::" src/kirito/*.hpp` → no hits). It is dead code left over
  from the pre-bytecode tree-walking evaluator (control flow is now compiled entirely to jumps —
  `Op::Jump`/`PopJumpIfFalse`/etc. — with no runtime `Flow` signal). Not a bug (harmless, header-only,
  costs nothing at runtime), but it is DRY-adjacent dead weight: a maintainer reading `control.hpp`'s
  comment ("Using a signal (not C++ exceptions) keeps the common path exception-free") would reasonably
  but incorrectly conclude the VM still dispatches on `Flow` values, when in fact the whole file is
  vestigial. [severity: LOW] [confidence: confirmed] — **Proposed fix**: delete `control.hpp` and its
  `#include` in `src/kirito.hpp:9` (N/A here, read-only).

## Verified-correct (adversarial, no bug — recorded so future rounds don't re-litigate)

- Deep bare recursion (`f(n) -> f(n+1)`) throws catchable "maximum recursion depth exceeded", no crash.
- Deep recursion routed through a higher-order native (`sorted(xs, key=deeplyRecursive)`) also throws
  catchably via the dual count/native-stack-bytes `CallGuard` (confirms the A04-1-from-v1.12 fix still
  holds in v1.15; see `.audit/v1.12.1`/`test_audit_v112.cpp`).
- `break` inside `with` inside `try`/`finally` inside a `for` loop: correct exit ordering, all `_exit_`/
  `finally` cleanups run, loop terminates immediately (verified `t1.ki`, `t14.ki`).
- 20000-iteration loop of nested `try`/`with`/`try`/`finally` + `continue`/`break` completes with no
  operand-stack drift/crash (`t2.ki`).
- 3-level-deep `break` crossing three nested `finally` blocks inside one loop iteration runs all three
  cleanups exactly once, in innermost-first order, then continues the loop correctly (`t4.ki`).
- `return` crossing `with A / with B / try-finally / with C` returns the correct value (`42`) with
  `_exit_`/`finally` firing in the right order (`t5.ki`).
- An exception thrown from `_exit_()` itself replaces the body's in-flight exception (no chaining) —
  by-design, consistent with the finally-throws-replaces behavior (`t6.ki`).
- `BuildClass`'s owned-clone method-install path (`klass.defineMethod` / `classEnv.define`) is correctly
  write-barriered (confirmed by code read: `class_value.hpp:78-81`, `environment.hpp:43-48`/`68`), and
  stays correct under `--gc-threshold 1` for repeated class construction, subclassing, `_super_`, and
  sibling-method calls (`t10.ki`, `t11.ki`).
- Nested `try/with/try/finally/catch/finally` × 6 loop iterations with `continue`/`break`/`throw` at
  different iterations produces byte-identical log output with and without `--gc-threshold 1` (`t14.ki`,
  `t15.ki`) — no GC-pressure-induced corruption of the block stack or operand stack.
- A class-definition statement re-executed inside a deeply recursive function (a `class Local:` body
  compiled/run once per call) does not bypass or interact badly with the call-depth guard — the guard
  still fires (bounded by the *calling* recursion, not by class-body execution cost) (`t9.ki`).
- `ClassValue`/`InstanceValue::selfHandle` are self-referential handles deliberately excluded from
  `children()` — correct (the object is already being traced; a self-edge is redundant, not a leak).
- Pre-allocation field writes in `Op::BuildClass` (`cls->base = base`, `cls->closure = classScope`,
  `cls->defineMethod(...)` before `vm_.alloc(std::move(cls))`) are barrier-safe: every `Object` is born
  YOUNG (`object.hpp:114`), so `gcWriteBarrier`'s `container->gcYoung()` early-out makes these calls
  no-ops until the object is actually promoted — no bug.

## Summary

One MED-severity, previously-known-but-still-unfixed bug re-confirmed: **A04-1**, `excSpan_` clobbered
by a nested try/catch fully resolved inside a `finally` (or a `with`-exit), producing a wrong line:col
in the traceback/error message for the exception that is actually propagating. This is a pure
diagnostics bug (the exception *value* is unaffected) but has now been flagged in three consecutive
rounds (v1.13 A05-2, v1.14 A04-3, this round's A04-1) without a fix landing, and the v1.14.1 re-scan
apparently missed it entirely — worth prioritizing a fix + regression test this time.

Beyond that, the bytecode VM's control-flow machinery (operand stack, block stack, jump targets,
finally/with-exit inlining, `BuildClass`'s owned-clone barrier, and the dual call-depth/native-stack
guard) held up under a wide adversarial sweep — deep nesting of try/with/loop/throw/break/continue/
return in every combination, 20000-iteration stress loops, deep recursion (bare and via higher-order
natives), and GC-every-allocation pressure (`--gc-threshold 1`) all produced correct, stable results.
Five coverage gaps (A04-G1..G5) are recorded — none are bugs, but the combinations they name are
exactly the kind that could hide a future regression (including a regression of A04-1 itself). One
dead-code item: `control.hpp`'s `Flow` enum is unreferenced anywhere in the codebase (vestigial from
the pre-bytecode tree-walker) and can be deleted.
