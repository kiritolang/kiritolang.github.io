# A03 — Compiler + Bytecode (v1.14)

Audit of `src/kirito/compiler.hpp`, `bytecode.hpp` (+ `resolver.hpp`, `analyzer.hpp`, `locals.hpp`
for how names/slots feed the compiler). Probing with `build-debug/ki`.

## v1.13 fix re-verification (all HOLD)

- **A03-1** (nested-default captures enclosing local): FIXED and correct. `locals.hpp`
  `CaptureScan::enterFunction` now scans nested-function defaults with `inNested=true`, growing
  `inner` over earlier params (lines 71-86). Reproduced both forms → both print `15` (was NameError
  / silent `105`). Real bug, correct fix.
- **A04-1** (return crossing finally that break/continues → operand leak): FIXED. `visit(ReturnStmt)`
  parks the return value in a hidden `$retN` slot when it crosses any Block-with-cleanup frame
  (compiler.hpp:333-354). Nested-loop repro now returns `[1, 2]` (was garbage). Real bug, correct fix.
- **A04-2 / A04-3** (negative/unary switch-case duplicate silently accepted; killed O(1) dispatch):
  FIXED. `constSwitchKey` constant-folds `UnaryExpr(Neg, numeric literal)` (compiler.hpp:168-182);
  duplicate `case -1`/`case -1` now throws "duplicate switch case value". Real bug, correct fix.

---

## New findings (v1.14)

### A03(v14)-1: `switch` case labels accept arbitrary runtime expressions (calls, arithmetic, names), diverging from the documented "constant scalars" contract
- severity: low
- location: `src/kirito/compiler.hpp` `visit(SwitchStmt)` chain path (lines 410-431);
  validation only at run time in `bytecode_vm.hpp:346` (SwitchMatch requires the *evaluated* value be
  a scalar). docs: `CLAUDE.md` ("Case labels are constant scalars"), `docs/pages/02-language-guide.md`.
- category: correctness (spec/doc divergence)
- description: The doc states case labels are **constant scalars** (Integer/Float/String/Bool/None).
  The implementation is far more permissive: any label whose `constSwitchKey` is nullopt (a function
  call `case g()`, arithmetic `case 1 + 1`, a name `case -x`, a double-negative `case - -1`) is
  accepted and evaluated **at run time** on the O(n) SwitchMatch chain, throwing only if the value
  turns out non-scalar. So `switch` is effectively a "cond" whose arms are arbitrary exprs compared to
  the subject. Two consequences: (a) a mis-typed label the user *intended* as a constant (e.g. a bare
  name) silently compiles as a runtime comparison instead of erroring; (b) semantic duplicates that
  aren't syntactic literals (`case 2` + `case 1 + 1`, both = 2) are **not** flagged as duplicate — the
  later arm is silently dead (no-fallthrough → first match wins), contradicting "duplicate case values
  are rejected". A single such label also silently downgrades the whole switch off the O(1)
  SwitchDispatch fast path to the O(n) chain (the residual of v1.13 A04-3, now only for genuinely
  non-const labels since negatives are folded).
- failure-scenario (CONFIRMED, all run clean, no diagnostic):
  `switch 1:` / `  case g():` (g returns 1) → "called";
  `switch 2:` / `  case 2:` "literal-two" / `  case 1 + 1:` "expr-two" → prints only "literal-two",
  the `1+1` arm dead but not flagged.
- proposed-test: decide the contract. If labels are meant to be constants, reject a non-const label at
  compile time (parser or compiler) with a clear message and add `errors/switch_nonconst_case.ki`. If
  the permissive behavior is intended, update the docs to say labels are exprs evaluated at run time
  (scalar-valued) and that only literal duplicates are detected — plus a `.ki` test pinning
  `case g()` / `case 1+1`.
- proposed-fix: flag only — needs a design decision (tighten impl vs. relax docs). Low risk either way.
- confidence: high (behavior reproduced).

### A03(v14)-2: const-dedup DISTINCTNESS still untested in C++ and `.ki` (only transparency is pinned)
- severity: coverage-gap
- location: `src/kirito/compiler.hpp` `scalarConstKey` (73-84); tests: `test_slot_locals.cpp:94-95`
  (transparency only), no distinctness test in `test_audit_v113.cpp` / `test_bytecode.cpp` / any `.ki`.
- category: coverage
- description: `scalarConstKey` keys floats on **exact bit_cast bits** (not roundtrip, unlike the
  switch keys) precisely so `-0.0`/`0.0`, `1`(Int)/`1.0`(Float), `True`/`1`, and distinct NaNs never
  collapse into one shared const slot. The v1.13 A04-6 gap remains open: the only test is transparency
  (repeated `"x"`/`1` produce correct values). A regression that keyed floats via `to_string` (6-digit)
  or dropped the type prefix would silently collapse `-0.0` and `0.0` (→ `[0.0, 0.0]` display) or `1`
  and `1.0`, and EVERY current test would still pass. I verified the behavior is currently correct:
  `[-0.0, 0.0]` → `[-0.0, 0.0]`, `[1, 1.0]` → `[1, 1.0]`.
- proposed-test: C++ (`test_bytecode.cpp`) + `.ki`: `run("[-0.0, 0.0]") == "[-0.0, 0.0]"`,
  `run("[1, 1.0]") == "[1, 1.0]"`, `run("[True, 1]") == "[True, 1]"`, and same-slot transparency for
  `[1,1,1]`/`["x","x"]`.
- proposed-fix: flag only (add the tests).
- confidence: high.

### A03(v14)-3: switch adversarial cases still untested in C++ (NaN subject/label, nested switch, -0.0/0.0 case collapse, empty-string vs None)
- severity: coverage-gap
- location: `test_switch.cpp` (covers exact-type, multi-value, default, non-scalar→default, literal
  duplicate, soft-keyword, control-flow propagation). Missing corners flagged in v1.13 A04-7.
- category: coverage
- description: Not pinned in any C++/`.ki` test: (a) a NaN subject and a `case NaN` label both →
  default (the `scalarSwitchKey` / `literalSwitchKey` NaN→nullopt branch); (b) `case -0.0` matching a
  `0.0` subject and vice-versa (the `d==0.0` collapse — the fast-path key must agree with `==`);
  (c) a nested `switch` inside a switch case body (exercises the `tblIdx`-stable-across-nesting
  reservation, compiler.hpp:392-393); (d) an empty-String label vs a None label (keys "S" vs "N").
  All verified currently correct by probing; a regression in the compile-time↔runtime key agreement
  would silently mis-dispatch and pass the existing suite.
- proposed-test: add the above to `test_switch.cpp` / a switch spec script (NaN via `math.nan`, since
  `0.0/0.0` throws division-by-zero and can't produce a NaN literal).
- proposed-fix: flag only.
- confidence: high.

### A03(v14)-4: [DRY, persisting] the scalar switch-key logic is still hand-triplicated across compiler.hpp and runtime.hpp
- severity: dry
- location: `compiler.hpp` `literalSwitchKey` (149-162) + `constSwitchKey` (168-182);
  `runtime.hpp` `scalarSwitchKey` (2330-2345). (v1.13 A04-12 — NOT addressed.)
- category: dry
- description: `literalSwitchKey` (compile time, over `LiteralExpr`) and `scalarSwitchKey` (run time,
  over a Handle) MUST agree byte-for-byte — same prefixes (`N`/`B0`/`B1`/`I`/`F`/`S`), same
  `floatToRoundtrip`, same `-0.0→0.0` collapse, same NaN→nullopt — or the compile-time SwitchDispatch
  table would mis-map at run time. I diffed both and they ARE currently identical (and I confirmed
  agreement empirically: 1 vs 1.0, -0.0/0.0, max int64, negative int, 0.1, -1.5 all dispatch
  correctly). But they are two independently-authored copies in different headers, plus a third
  near-parallel `scalarConstKey` (intentionally different — bit_cast, no collapse — for const
  identity). The negative-literal fold (v1.13 A04-2) had to be added to `constSwitchKey` and kept in
  lockstep with the runtime negation — exactly the drift hazard. Recommend factoring the switch key
  into one shared `scalarSwitchKeyFromParts(...)` helper both sites call, with a comment marking
  `scalarConstKey` as intentionally distinct.
- proposed-fix: extract a single shared helper for the switch key; leave the const key separate + commented.
- confidence: high.

### A03(v14)-5: [efficiency, persisting] addConst boxes every literal occurrence then discards it on a dedup hit
- severity: low
- location: `compiler.hpp` `visit(LiteralExpr)` (561-572) unconditionally calls `vm_.makeInt/makeFloat/
  makeBool/makeString`; `addConst` (59-70) computes the dedup key AFTER and returns the existing slot,
  dropping the fresh Handle. (v1.13 A04-4 — NOT addressed.)
- category: perf / minor GC hygiene
- description: A function with N repeated `1`/`"x"` literals allocates N throwaway boxes (each an
  immediately-dead, unrooted Object) even though only the first is kept. Correctness is fine (the
  deduped slot points at the first, rooted Handle); it just defeats part of the point of compile-time
  const-dedup and litters the arena until the next GC. A `literalConstKey(const LiteralExpr&)` computed
  from the AST (mirroring `literalSwitchKey`) would let `visit(LiteralExpr)` box only on a cache miss.
- proposed-fix: compute the dedup key from the AST literal; `makeX` only on a miss.
- confidence: high (by inspection; efficiency not correctness).

---

## Summary (A03 compiler+bytecode, v1.14)

- **v1.13 fixes: ALL HOLD and are correctly implemented + well-covered.** A03-1 (nested-default
  capture), A04-1 (return-crossing-finally operand leak), A04-2/A04-3 (negative switch case) each
  reproduce as fixed AND have dedicated regression tests in BOTH `test_audit_v113.cpp` and
  `spec_audit_v113.ki`. The v1.13 coverage gaps A04-5 (value-carrying finally/with exits) are closed.
- **No new confirmed BUGS.** I ran an exhaustive adversarial matrix — return/break/continue crossing
  {finally, with, nested finally, finally-in-with, switch-in-loop} under {single, nested} loops;
  exceptions crossing for/while loops and mid-switch-subject; _enter_/_exit_ throwing on every path;
  a kitchen-sink for/while/switch/try/with/if combo — every result was correct and the operand stack
  never leaked or under-popped. The compiler's control-flow lowering and slot-addressed locals are
  robust. All three scalar-key implementations agree empirically.
- **Findings**: 1 low correctness/doc divergence (A03(v14)-1, switch labels accept runtime exprs);
  2 coverage-gaps (A03(v14)-2 const-dedup distinctness; A03(v14)-3 switch adversarial); 1 DRY
  (A03(v14)-4, persisting triplicated switch-key logic); 1 low perf (A03(v14)-5, persisting
  allocate-then-discard).

Status: COMPLETE.
