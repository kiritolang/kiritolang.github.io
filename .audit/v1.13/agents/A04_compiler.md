# A04 — Compiler (AST→bytecode) + bytecode format

Audit of `src/kirito/compiler.hpp` and `src/kirito/bytecode.hpp` (with `src/kirito/locals.hpp`
and cross-checks against `src/kirito/bytecode_vm.hpp`, `runtime.hpp`, `parser.hpp`, `ast.hpp`).
Read-only static audit. Confirmed bugs first, then coverage gaps, then DRY notes.

Note: the string `A04-2` already appears as a marker in `bytecode_vm.hpp:394` from a PRIOR audit
round; my numbering below is fresh (A04-1..) and unrelated to that legacy tag.

## Compiler surface (reference for the findings)

- Ops (bytecode.hpp): 41 opcodes, each `Instr{op, a:uint32_t, span}`. Side-tables on `Proto`:
  `consts` (scalar Handles, GC-pinned), `names` (identifiers/attrs/format-specs), `funcs`
  (MakeFunction), `calls` (CallSpec = positional count + kw-name list), `unpacks` (UnpackSpec =
  count + starIndex), `classes` (ClassStmt*), `switches` (SwitchTable), `localCount`, `localNames`.
- Lowering: if→PopJumpIfFalse chain; while/for→GetIter/ForIter + break/continue patch lists in
  `CFrame` Loop frames; and/or→JumpIf{False,True}OrPop (peek-based short-circuit); conditional-expr
  →PopJumpIfFalse/Jump; switch→O(1) SwitchDispatch table when ALL case values are literal scalars,
  else O(n) SwitchMatch/PopJumpIfTrue chain; try/with→SetupBlock/PopBlock runtime block stack +
  inline finally/exit duplicated per exit path; break/continue/return unwind Block frames inline via
  `unwindFramesAbove`; unpack→Unpack op + per-target store; kwargs→CallSpec; class body harvested via
  a separate cached Proto (protoForBody); parameter defaults→own Proto (protoForExpr); f-strings→
  LoadConst/FormatValue/BuildString.
- Const dedup: `scalarConstKey` keys None/Bool/Int(dec)/Float(**bit_cast bits**)/String(raw). Floats
  keyed on exact bits so -0.0/0.0 and distinct NaNs stay in SEPARATE slots (correct). Switch keys use
  a DIFFERENT scheme (`literalSwitchKey`/`scalarSwitchKey`: floatToRoundtrip + -0.0/0.0 collapse +
  NaN→none) — intentional divergence (see A04-7 DRY note).
- Slots: `assignLocalSlots` slots a function's non-captured, non-parameter locals; captured names
  (from `capturedLocals` in locals.hpp) and params stay name-based. Hidden `$withN`/`$excN` locals
  slotted inside functions, name-based elsewhere.
- Compile-time errors thrown as KiritoError: deep nest (DepthScope, kMaxDepth=3000), invalid
  assignment target, two starred targets, starred expr outside assignment, and the *deferred*
  (run-time-observable) throws for positional-after-keyword and duplicate switch case.

---

## CONFIRMED BUGS

### A04-1: `return EXPR` crossing a `finally` that does `break`/`continue` leaks the return value on the operand stack → enclosing-loop iteration corrupted / wrong output
- **severity:** HIGH
- **location:** `compiler.hpp` `visit(ReturnStmt)` (lines 313-318) + `unwindFramesAbove` (687-699) +
  `visit(BreakStmt)`/`visit(ContinueStmt)` (289-299); interacts with `visit(ForStmt)` cursor unwind
  (278, `CFrame.unwind=1`) and VM `ForIter` peek(0) (`bytecode_vm.hpp:314-327`).
- **category:** operand-stack imbalance / silent mis-compile
- **description:** `visit(ReturnStmt)` compiles the return value (pushing it on the operand stack),
  THEN calls `unwindFramesAbove(0)`, which runs each crossed `try`'s `finally` body inline **with the
  return value still on the operand stack**. The exception path was explicitly hardened against
  exactly this (comment lines 419-423; `emitFinallyExc` parks the in-flight exception in a hidden
  slot so the finally runs at a clean operand height). The **normal/return-crossing path was NOT
  hardened**: `emitFinally` (the cleanup registered on the finally `CFrame`, line 438/448) runs with
  the return value un-parked beneath it. If that finally body performs `break`/`continue`, the
  break's cursor-unwind `for (i<frames_[li].unwind) emit(Op::Pop)` (line 292) pops the wrong slot —
  it removes the **return value** instead of the for-cursor, leaving the cursor orphaned on the
  stack. An enclosing loop's `ForIter` then does `peek(0)` on the **leaked cursor** and iterates the
  wrong iterator.
- **failure-scenario:**
  ```
  var f = Function():
      var out = []
      for i in [1, 2]:
          for j in [10, 20, 30]:
              try:
                  return 99
              finally:
                  break        # abandons the return, breaks the inner loop
          out.append(i)
      return out
  f()                          # expected [1, 2]; actual: garbage (outer ForIter reads the leaked
                               # inner cursor, so i takes values 20/30..., out is wrong / over-long)
  ```
  The single-loop variant (`for i in [...]: try: return 99 finally: break` then `return -1`) happens
  to produce the correct value only because nothing re-reads the stale slot before the frame is torn
  down — which is why the bug is invisible in `spec_finally_control.ki` (that file tests
  break/continue/return reached via an EXCEPTION, and `return` *inside* a finally, but never
  `return EXPR` in the try BODY with a `break`/`continue` in the finally under nested loops).
- **proposed-test:** the nested-loop program above asserting `f() == [1, 2]`; plus a
  continue-in-finally variant and a `while`-nested variant; add to `spec_finally_control.ki` /
  `test_bytecode.cpp`.
- **proposed-fix:** mirror the exception path: when `unwindFramesAbove` runs a `finally` cleanup on a
  value-carrying exit (return), park the pending value in a hidden slot across the cleanup and reload
  it after (as `emitFinallyExc` already does for the exception value). Simplest: have the ReturnStmt
  path store the return value into a hidden `$ret` slot before `unwindFramesAbove` and reload it
  before `Op::Return`, so every crossed finally runs at clean height.
- **confidence:** high (static trace against the VM's ForIter/peek and CFrame.unwind semantics).

### A04-2: duplicate **negative / unary** switch case values are silently accepted (not rejected)
- **severity:** MEDIUM
- **location:** `compiler.hpp` `visit(SwitchStmt)` duplicate scan (lines 334-347) — only inspects
  `dynamic_cast<const ast::LiteralExpr*>`.
- **category:** silent acceptance of a program that should error (spec divergence)
- **description:** A negative numeric label like `-1` is parsed as `UnaryExpr(Neg, LiteralExpr(1))`,
  NOT a `LiteralExpr` (`parser.hpp:543-552`). The duplicate-case detector only keys `LiteralExpr`
  values, so `case -1: ... case -1:` (and `case -0.0`, `case -1.5`) is never seen as a duplicate.
  Because a non-literal label also flips `allLiteral=false`, the switch falls to the SwitchMatch
  comparison chain where the first matching arm wins and the duplicate arm is silently dead. The doc
  ("duplicate case values ... are rejected") and the positive-literal behavior (`errors/
  switch_duplicate_case.ki` → "duplicate switch case value") are thus violated for negative labels.
- **failure-scenario:** `switch 1:\n  case -1:\n    discard 1\n  case -1:\n    discard 2` compiles &
  runs with no diagnostic; the second `case -1` is dead. Positive `case 5 / case 5` errors.
- **proposed-test:** an `errors/switch_duplicate_negative.ki` expecting "duplicate switch case value";
  also `case -0.0 / case 0.0` (they share a runtime key) should be flagged.
- **proposed-fix:** in the duplicate scan (and the `litKey`/`allLiteral` logic), constant-fold a
  `UnaryExpr(Neg, numeric literal)` into a negative scalar key so negative labels are treated as
  literal scalars (also fixes A04-3). Alternatively fold negatives in the parser.
- **confidence:** high.

### A04-3: any non-literal (e.g. negative) case label silently downgrades the WHOLE switch off the O(1) SwitchDispatch to the O(n) comparison chain
- **severity:** LOW (performance, silent)
- **location:** `compiler.hpp` `visit(SwitchStmt)` `allLiteral` gate (lines 353-357).
- **category:** silent performance regression / inconsistency
- **description:** `allLiteral` is false if *any* case value is not a `LiteralExpr`. A single
  `case -1` (unary-minus, per A04-2) forces every arm of that switch — including plain `case 1`,
  `case None`, `case "x"` — onto the linear SwitchMatch chain, losing the documented O(1) dispatch
  the feature advertises. Confirmed reachable: `r7_language.ki:327` (`case -1` alongside
  `case None/True/False`) runs entirely on the chain path.
- **failure-scenario:** a large switch with many literal arms plus one `case -1` is O(n) per dispatch
  though it reads as all-literal.
- **proposed-test:** a bytecode/behavior test asserting a switch with a negative label still returns
  correct results (behavioral), plus (if a Proto inspector exists) that it emits SwitchDispatch after
  the fold fix.
- **proposed-fix:** same constant-fold as A04-2 — treat `-<numeric literal>` as a literal scalar so
  such switches keep the fast path.
- **confidence:** high.

### A04-4: `addConst` allocates a fresh boxed value for every literal occurrence, then discards it on a dedup hit
- **severity:** LOW (efficiency; also a transient unrooted handle)
- **location:** `compiler.hpp` `visit(LiteralExpr)` (528-539) calls `vm_.makeInt/makeFloat/makeBool/
  makeString` unconditionally; `addConst` (59-70) discards the freshly-made Handle when
  `scalarConstKey` already maps to a slot.
- **category:** DRY/efficiency; minor GC hygiene
- **description:** Each repeated `1`/`"x"` literal allocates a new Object via `makeInt`/`makeString`
  before `addConst` computes the key and returns the existing slot, throwing the new allocation away
  (it is never pushed to `consts` nor `pushTemp`'d — an immediately-dead handle). Correctness is
  fine (the deduped slot points at the first, rooted handle), but it defeats part of the point of
  const-dedup (avoiding per-occurrence boxing at compile time) and litters the arena with garbage
  until GC.
- **failure-scenario:** compiling a function with thousands of repeated literals allocates thousands
  of throwaway boxes.
- **proposed-fix:** compute the dedup key from the AST literal (a `literalConstKey(LiteralExpr)`
  analogous to `literalSwitchKey`) and only `makeX` on a cache miss.
- **confidence:** high (behavior is by inspection; "bug" is efficiency, not correctness).

---

## COVERAGE GAPS

### A04-5: no test exercises the finally/with cleanup paths under a *value-carrying* exit
- **severity:** MEDIUM (this gap is exactly what hides A04-1)
- **location:** tests: `spec_finally_control.ki`, `test_bytecode.cpp` (lines 76-83).
- **description:** Existing finally tests cover: break/continue/return in a finally reached via an
  EXCEPTION; `return` *inside* a finally; normal-path break-in-finally; handler-rethrow-runs-finally.
  MISSING: `return EXPR` in a try BODY with a `break`/`continue` in the finally (A04-1), especially
  under nested loops; and break/continue/**return crossing a `with`** (only normal-completion `with`
  is tested — `amarg_with.ki`, `test_bytecode.cpp:83`). The `with`-exit cleanup is stack-neutral by
  construction (emitExit pushes+consumes the manager, pops the exit result), but there is no test
  proving break/continue/return crossing a `with` runs `_exit_` exactly once and returns correctly.
- **proposed-test:** matrix of {break, continue, return-value} × {finally, with, finally-inside-with}
  × {single loop, nested loops}, each asserting both the RESULT and (via a side-effect log) that
  cleanups ran the right number of times.
- **confidence:** high (gap verified by reading every finally/with test).

### A04-6: const-dedup distinctness edge cases are unpinned
- **severity:** LOW
- **location:** `scalarConstKey` (compiler.hpp 73-84); tests: `test_slot_locals.cpp:93-95`.
- **description:** The only const-dedup test asserts repeated `"x"`/`1` literals produce correct
  values (transparency). No test pins the DISTINCTNESS guarantees the bit-cast float keying exists
  for: `-0.0` vs `0.0` must remain separate const slots (they are `==` but must round-trip exact bits
  and be distinct Set/Dict keys); `1` (Integer) vs `1.0` (Float) must be distinct slots (keys "I1"
  vs "F<bits>"); `True` vs `1` distinct ("B1" vs "I1"); `None` vs `"N"`/`""` String distinct. A
  regression collapsing any of these (e.g. keying floats via `to_string`) would silently corrupt
  output and pass every current test. (Distinct-NaN literals are effectively unreachable from source
  — no NaN float literal spelling — so the NaN branch of `scalarConstKey` is defensive/dead, worth a
  comment but not a test.)
- **proposed-test:** `run("[-0.0, 0.0]") == "[-0.0, 0.0]"`, `run("var s={0.0}\ns.add(-0.0)\nlen(s)")`
  distinctness, `run("[1, 1.0]") == "[1, 1.0]"`, `run("[True, 1]")`, plus a same-slot check that
  `[1,1,1]`/`["x","x"]` still work.
- **confidence:** high.

### A04-7: `switch` compiler-side branches thinly covered from adversarial angles
- **severity:** LOW
- **location:** `visit(SwitchStmt)` (330-399); tests: `test_bytecode.cpp:61-64`, `r7_language.ki`.
- **description:** Covered: exact type match (`case 1` ≠ `case 1.0`), multi-value case, default,
  positive duplicate error, negative-label match (chain path), None/Bool labels. NOT covered:
  (a) NaN subject and NaN label → default (the `literalSwitchKey` NaN→`nullopt` / chain path and the
  VM `scalarSwitchKey` NaN→default) — asserting `switch NaN` and `case NaN` never match; (b) the
  *non-literal case value* fallback chain returning to `default` for a non-scalar subject with
  non-literal case exprs; (c) a nested switch inside a switch case body (exercises the
  `tblIdx`-stable-across-nesting reservation at lines 359-360, 373 — currently correct but untested);
  (d) `case -0.0` vs `case 0.0` collapsing (A04-2); (e) an empty-string String label vs a None label
  (key "S" vs "N").
- **proposed-test:** add the above cases (NaN via `0.0/0.0` or `math.nan`, nested switch) to a switch
  spec script.
- **confidence:** high.

### A04-8: compile-time-error throws for starred-expr-outside-assignment are untested (possibly parser-unreachable)
- **severity:** LOW
- **location:** `visit(StarExpr)` (650-652), `visit(TupleExpr)` star scan (642-645), and the
  "two starred targets" throw in `visit(AssignStmt)` (200-204 — note `errors/unpack_two_stars.ki`
  covers a VarDecl form; the AssignStmt tuple form path at line 202-203 is a *separate* code site).
- **description:** No test drives `visit(StarExpr)`/`visit(TupleExpr)` "starred expression is only
  valid as an assignment target" (e.g. `var t = *xs`, `discard 1, *xs`, `f(*xs)`) — if the parser
  rejects star in value position first, these are dead defensive throws (worth confirming and
  documenting); if reachable, they need an `errors/*.ki`. The AssignStmt "two starred targets in
  assignment" (line 202) is distinct from the VarDecl "two stars" path exercised by
  `unpack_two_stars.ki`; `a, *b, *c = xs` should have its own error test.
- **proposed-test:** `errors/star_in_value.ki` (each spelling) and `errors/assign_two_stars.ki`
  (`a, *b, *c = [1,2,3]`).
- **confidence:** medium (whether the throws are reachable depends on the parser's star handling).

### A04-9: compile-time depth guard (kMaxDepth=3000) is unreachable behind the parser's 2000 cap
- **severity:** LOW (dead safety net; and an implicit coupling)
- **location:** `compiler.hpp` `DepthScope`/`kMaxDepth=3000` (166-177); parser
  `kMaxParseDepth=2000`/250-sanitizer (`parser.hpp:418-422`), applied to block nesting via
  `parseIndentedSuite` (`parser.hpp:328-332`).
- **description:** Every AST reaching the compiler was produced by the parser, which caps block AND
  expression nesting at 2000 (250 under sanitizers) — strictly below the compiler's 3000. So the
  compiler's `DepthScope` throw ("expression too deeply nested to evaluate") can never fire for
  parser-produced input; `errors/deep_source.ki` triggers the *parser* message, not the compiler's.
  Separately, `locals.hpp` `collectBlockDecls`/`CaptureScan` recurse over the AST with **no
  independent depth guard**, relying entirely on the parser's cap to keep native recursion bounded
  (≤2000). This is a silent coupling: if the parser cap were ever raised or a non-parser AST source
  added, `assignLocalSlots`→`capturedLocals` could stack-overflow before the compiler's guarded pass
  runs (it executes first, at `compile()` line 39).
- **proposed-test/fix:** document the coupling; optionally add a DepthScope-style guard to the
  locals.hpp recursions, or a static_assert that compiler kMaxDepth ≥ parser cap is intentional (it
  is currently the reverse). No functional test possible without lowering the compiler guard.
- **confidence:** high.

### A04-10: no direct coverage of tuple-assign to index/member targets or evaluation order
- **severity:** LOW
- **location:** `visit(AssignStmt)` tuple path (196-213) + `compileAssignTarget` (218-236).
- **description:** The unpack-into-mixed-targets layout (value pushed under obj/keys so `SetItem`
  reads value at `stack[base-2]`) is subtle and correct by inspection, but tests only cover
  name/swap/starred targets (`test_slot_locals.cpp:57-59`). Untested: `a[0], a[1] = x, y`,
  `obj.p, obj.q = 1, 2`, mixed `a, b[i], c.m = 1, 2, 3` with a starred target, and left-to-right
  evaluation of target subexpressions. A regression in the SetItem/SetAttr operand order would slip
  through.
- **proposed-test:** golden `.ki` covering index/member unpack targets (incl. starred + mixed).
- **confidence:** high.

### A04-11: f-string, format-spec, and BuildString edge cases lightly tested at the compiler level
- **severity:** LOW
- **location:** `visit(FStringExpr)` (654-664).
- **description:** Empty f-string (`f""` → BuildString 0 or 1), an f-string that is all-literal (no
  expr parts), an empty format spec vs an explicit one (`FormatValue` names[a]=="" means "none"), and
  a format-spec string that is deduped in the `names` table alongside a real identifier of the same
  text (index is per-opcode so this is safe, but untested). Also `BuildString` count == parts.size()
  invariant (each part nets exactly one String) has no direct assertion.
- **proposed-test:** `f""`, `f"a{1}b{2}c"`, `f"{x}"` vs `f"{x:}"` vs `f"{x:>5}"`.
- **confidence:** medium.

---

## DRY / STRUCTURAL NOTES

### A04-12: the scalar-key logic is triplicated across two files (compiler.hpp + runtime.hpp), a divergence hazard
- **severity:** LOW (maintainability)
- **location:** `compiler.hpp` `scalarConstKey` (73-84) and `literalSwitchKey` (149-162);
  `runtime.hpp` `scalarSwitchKey` (2285-2300).
- **description:** Three near-parallel scalar-to-key functions exist. `literalSwitchKey` (compile
  time, over `LiteralExpr`) and `scalarSwitchKey` (run time, over a Handle) **MUST agree exactly**
  (both: floatToRoundtrip, `-0.0`/`0.0` collapse, NaN→none, "N"/"B0/1"/"I"/"F"/"S" prefixes) or the
  SwitchDispatch table built at compile time would mis-map at run time — yet they are independently
  authored in different files/headers. `scalarConstKey` deliberately differs (Int via to_string,
  Float via **bit_cast** not roundtrip, no -0.0 collapse) because const identity needs bit-exactness
  — this divergence is correct but easy to conflate with the switch keys. The two switch-key
  functions should share one implementation (e.g. a common `scalarKey(kind, value...)` helper) to
  guarantee they can never drift; the const key should stay separate but be commented as
  intentionally distinct. Note the A04-2/A04-3 fixes must update `literalSwitchKey` AND keep it in
  lockstep with `scalarSwitchKey`.
- **confidence:** high.

### A04-13: CaptureScan / collectBlockDecls verified complete over all AST node types (no under-approximation) — positive finding
- **severity:** INFO
- **location:** `locals.hpp`.
- **description:** Under-approximating captures (marking a genuinely-captured local as non-captured →
  slotted) would be a correctness bug (a nested closure would miss the binding). I enumerated all 17
  Expr and 18 Stmt node types in `ast.hpp` and confirmed `CaptureScan::scanExpr`/`scanStmt` and
  `collectStmtDecls` handle every scope-relevant one (Literal/Break/Continue/Pass/Todo correctly bind
  and reference nothing). Defaults are scanned in the enclosing scope; class base too. No missing
  node type found. Over-approximation is safe. No action; documented so a future AST node addition is
  flagged as needing a locals.hpp update.
- **confidence:** high.

---

## Summary
- Confirmed bugs: **4** — A04-1 (HIGH, finally/return operand-stack leak → wrong output),
  A04-2 (MEDIUM, negative duplicate case silently accepted), A04-3 (LOW, negative label kills O(1)
  switch), A04-4 (LOW, const-dedup allocate-then-discard).
- Coverage gaps: A04-5..A04-11 (finally/with value-carrying exits; const-dedup distinctness;
  switch adversarial; starred-expr errors; depth-guard coupling; index/member unpack targets;
  f-string edges).
- DRY: A04-12 (triplicated scalar-key logic; switch keys must stay lockstep across compiler/runtime);
  A04-13 (capture analysis verified complete — positive).

Status: COMPLETE
