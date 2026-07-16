# A04 — bytecode VM + runtime dispatch + exceptions (v1.15.1)

Scope: `src/kirito/bytecode_vm.hpp`, `src/kirito/runtime.hpp`, `src/kirito/function.hpp`.
Probing binary: `./build-debug/ki` (no builds run — per round rule).

Status: IN PROGRESS

## Findings

### A04-1: traceback frame line disagrees with the `error:` line whenever a `finally`/`with` reraises  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/bytecode_vm.hpp:518-521` (`appendFrame`) vs `:426` (`Op::Reraise`)
- What: `appendFrame` derives the frame's line from `code[ip-1].span` — the instruction that was
  executing. For an implicit **Reraise** (emitted after a `finally` body, or after a `with`'s
  `_exit_`), that instruction's span is the **`try:` / `with` statement's** span, not the origin of
  the exception. The exception VALUE carries the true origin (`Op::Reraise` throws
  `KiritoThrow{v, excSpan_}` — the v1.15 `excSpan_` parking works), so the `error:` line:col is
  right while the traceback frame printed directly above it is wrong. The two lines of the same
  diagnostic contradict each other.
- Repro (`/tmp/a04/p5.ki`, `throw` is on line 4, `try:` on line 3):
```
var io = import("io")
var f = Function():
    try:
        throw "BOOM"
    finally:
        io.print("cleanup")
f()
```
```
$ ./build-debug/ki p5.ki
cleanup
Traceback (most recent call last):
  File "p5.ki", line 7, in <module>
  File "p5.ki", line 3, in f          <-- the `try:` line, not the throw
p5.ki:4:9: error: uncaught exception: BOOM     <-- correct origin
```
  Without the `finally` the same program is self-consistent (`/tmp/a04/p6.ki`): frame `line 3`,
  error `3:5` — both the throw.
  The `with` form has the identical defect (`/tmp/a04/p7.ki`, `throw` on 9, `with` on 8):
```
exit
Traceback (most recent call last):
  File "p7.ki", line 10, in <module>
  File "p7.ki", line 8, in f          <-- the `with` line, not the throw
p7.ki:9:9: error: uncaught exception: WITHBOOM
```
  An **explicit** `throw e` from a `catch` is correct (`/tmp/a04/p8.ki`): frame line 7 == error 7:9,
  because that is a genuinely new throw site.
  Also reachable while handled, via `sys.traceback()` (`/tmp/a04/p2.ki`): reports `line 5` (`try:`)
  for a throw on line 6.
- Impact: anyone reading a traceback for an exception that crossed a `finally` or a `with` — i.e.
  most real resource-handling code — is pointed at the wrong line. Silent: the number is plausible,
  just off, and it is the frame most likely to be trusted (the innermost).
- Proposed fix: in the `catch (KiritoThrow& t)` arm, when `t.traceback` is still empty (this is the
  frame where the exception originated), prefer `t.span.line` over `code[ip-1].span.line`. i.e. pass
  the known origin span into `appendFrame` and let it use that for the innermost frame only; outer
  frames must keep `code[ip-1]` (the call site). Breaks no documented contract — it makes the
  traceback agree with the already-correct `error:` line.
  Caveat to check when fixing: the `KiritoError` arm's `e.span` can come from a different chunk
  (a native/frozen-module error), so restricting the change to the `KiritoThrow` arm (or to
  `span.line != 0`) is the conservative shape.
- Proposed test: `tools/tests/scripts/` (a traceback golden) or `tools/tests/errors/` — assert the
  innermost frame line of a throw inside `try/finally` equals the `throw` line, not the `try` line.
  Name it for the symptom, e.g. `traceback_line_through_finally`.

### A04-2: a bound method backed by a NATIVE function silently DISCARDS every keyword argument (including unknown ones)  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/runtime.hpp:1717-1732` (`makeBoundMethod`), specifically line 1729
  `return m.call(vm, full);  // native method: positional only`
- What: `makeBoundMethod` builds the kwarg-aware callable used by **`InstanceValue::getAttr`**
  (runtime.hpp:1751) and **`SuperValue::getAttr`** (runtime.hpp:1778). Its lambda receives `named`,
  but forwards it only when the method is a `ValueKind::Function`. For a `ValueKind::NativeFunction`
  member it calls `m.call(vm, full)` — which drops `named` **on the floor** and, because it bypasses
  `applyCall`, also bypasses `NativeFunction::bindArgs`. So on this path:
  1. a **known** keyword is silently ignored (the parameter keeps its default),
  2. an **unknown/misspelled** keyword raises no error at all,
  3. defaults are never filled and no arity check runs — the impl gets whatever raw span it is handed.
  This breaks the CLAUDE.md contract that keywords work "**uniformly across every callable**", and
  it fails *silently*, which is the worst shape: a typo'd kwarg is accepted and ignored.
- Repro (`/tmp/a04/kwdrop.ki`):
```
var io = import("io")
class C:
    var f = format
var c = C()
io.print("c.f(spec='.2f')      -> " + String(c.f(spec = ".2f")))
io.print("c.f(zzz='nonsense')  -> " + String(c.f(zzz = "nonsense")))
try:
    io.print("  " + String(format(c, spec = ".2f")))
catch as e:
    io.print("  !! " + e)
try:
    io.print("  format(c, zzz='x') -> " + String(format(c, zzz = "x")))
catch as e:
    io.print("  !! " + e)
```
```
$ ./build-debug/ki kwdrop.ki
c.f(spec='.2f')      -> <C object>        <-- spec SILENTLY IGNORED
c.f(zzz='nonsense')  -> <C object>        <-- unknown kwarg SILENTLY IGNORED
direct format(c, spec='.2f') ->
  !! format type 'f' needs a number, not 'C'          <-- the same native, called directly: honoured
  !! format() got an unexpected keyword argument 'zzz' <-- and unknown kwargs are rejected
```
  The `bindArgs` bypass is visible too (`/tmp/a04/bm.ki`): `c.f(3.14159, spec = ".2f")` reports
  `format spec must be a String` (the impl saw `spec = 3.14159`), where `applyCall`+`bindArgs` would
  have reported `format() got multiple values for argument 'spec'`.
- Impact: primarily **embedders** — the documented extension path is to register a `NativeFunction`
  and install it as a class method; every keyword call against such a method silently loses its
  kwargs and skips arity/default binding. Also any Kirito class that adopts a builtin/stdlib function
  as a member. Not a memory-safety issue in practice: I fuzzed all 222 signatured stdlib natives
  through this path with only the receiver as an argument (`/tmp/a04/fuzzbm.ki`, generated from
  `inspect` over 18 modules) — `ok=19 err=203`, no crash, so the impls are individually defensive
  about their span length. The defect is the silent kwarg loss, not an OOB read.
- Proposed fix: make `makeBoundMethod` delegate to the single source of truth instead of
  hand-rolling the dispatch — the whole lambda body becomes
  `return applyCall(vm, methodH, full, named);`. That routes a Kirito function to `callFull` (as
  today), a kwarg-aware native to `callKw`, a signatured native through `bindArgs` (kwargs +
  defaults + arity), and a signatureless native with kwargs to the existing clear
  `"this callable does not accept keyword arguments"` error. It needs a forward declaration of
  `applyCall` above line 1717 (it is defined at :2271). Also a DRY win: the call protocol then lives
  in exactly one place. Breaks no documented contract — it *restores* one.
- Proposed test: `tools/tests/unit/test_calls*.cpp` or a `.ki` golden. Assert that a native function
  installed as a class member (a) rejects an unknown keyword with
  `got an unexpected keyword argument`, and (b) honours a known keyword. Name it for the symptom,
  e.g. `native_class_member_keyword_not_silently_dropped`.

## Log

- Read `bytecode_vm.hpp`, `function.hpp`, `bytecode.hpp` in full; `runtime.hpp`'s dispatch core
  (`applyCall`/`applyBinaryOp`/`applyUnaryOp`/`evalMemberGet`/`checkPrivateAccess`/`spreadValues`/
  `KiFunction::callFull`/`ClassValue::callFull`/`makeBoundMethod`/`isInstanceOf`); `compiler.hpp`'s
  `TryStmt`/`WithStmt`/`ForStmt`/`WhileStmt`/`Break`/`Continue`/`Return`/`Switch` codegen and
  `CFrame`/`unwindFramesAbove`.
- Probed: break/continue in `with` in `for`; recursion via 8 dunders; call/kw protocol across 10
  callable kinds; defaults semantics; finally/with swallow semantics; operand-stack leak stress.
- Found A04-1 (traceback line through finally/with). Everything else below checked out — see
  Non-findings.
