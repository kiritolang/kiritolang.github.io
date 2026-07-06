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
</content>
</invoke>
