# A02 Audit: resolver + analyzer + locals

Subsystem: `src/kirito/resolver.hpp`, `src/kirito/analyzer.hpp`, `src/kirito/locals.hpp`
Status: IN PROGRESS

Findings appended incrementally below.

## Context / prior-work status
- A03-1 (v1.13, nested-default capture) is FIXED in current locals.hpp (`enterFunction` scans defaults at `inNested=true`, line 82). Verified in code.
- A03-2 (v1.13, `declare` resets `used=false` on re-declaration) — analyzer.hpp:74-76 STILL resets unconditionally; prior "todo" finding, not re-reported here.
- A03-3 (v1.13, `lookupFunc` skips closer non-function shadowing binding) — analyzer.hpp:83-89 unchanged; prior "todo", not re-reported.
- All three AST walks (resolver checkExpr/Stmt, analyzer analyzeExpr/Stmt, locals scanExpr/Stmt) verified in sync across the full node set (17 Expr, 18 Stmt). A03-15 DRY concern stands but no active divergence.

### A02-1: Read-before-assign of a function local silently resolves to an enclosing/module binding of the same name (masks shadowing bugs)
- severity: medium
- location: src/kirito/resolver.hpp:35-72 (membership scoping) + the runtime "unwritten slot falls back to name lookup" path (compiler/bytecode_vm); resolver has no read-before-write detection
- category: correctness / design footgun
- description: Resolution is by scope MEMBERSHIP: `var x` makes `x` visible throughout the whole scope, so the resolver accepts a reference to `x` that lexically precedes its `var x` declaration. At runtime the local slot (or `vars_` entry) is unwritten, and the read TRANSPARENTLY falls back to a name lookup that climbs to the enclosing/module scope. Consequence: a genuine bug — using a local before assigning it — does NOT raise (as Python's UnboundLocalError would); instead, if an enclosing scope happens to bind the same name, the read silently returns that OUTER value. This is confirmed identical on both the slotted and the captured/name-based path (so the "byte-for-byte unchanged" claim holds), but the shared behavior itself is the footgun.
- failure-scenario:
  ```
  var y = 42
  var F = Function():
      io.print(y)      # intended g's local; prints 42 (module y), no error
      var y = 5
      return y
  discard F()
  ```
  prints `42`. With NO outer `y`, the same program throws runtime `name 'y' is not defined` (not a compile error, and not an "unbound local" message). So the diagnostic quality also depends on whether an outer same-name binding exists.
- proposed-test: a `.ki` golden asserting the module-shadow case prints the OUTER value (pins current behavior), plus an errors/*.ki asserting the no-outer case throws at the read line. If the design intent is UnboundLocalError semantics, that is a larger compiler change (track first-assignment dominance) — flag for a design decision.
- proposed-fix: design decision required. Option A: document + test current membership/fallback semantics as intentional. Option B: have the compiler emit an "unbound local" check for a local read that is not dominated by an assignment (Python-style). Given reference-assignment + membership scoping, Option A is likely intended; the risk is undocumented masking of shadow bugs.
- confidence: high (behavior), medium (whether it's considered a bug vs by-design)

### A02-2: Unused-result false positive on the short-circuit / conditional-expression side-effect idiom
- severity: low
- location: src/kirito/analyzer.hpp:239-240 (`producesUnusedValue`: `LogicalExpr` and `ConditionalExpr` unconditionally return true)
- category: analyzer false positive
- description: `producesUnusedValue` is deliberately smart for a bare CALL — it only flags a call whose callee is a locally-defined function that ALWAYS returns a value (`alwaysReturnsValue`), so a bare `doThing()` where `doThing` returns None is (correctly) NOT flagged. But when that same side-effecting call is an operand of a `LogicalExpr` (`cond and doThing()`) or a `ConditionalExpr` (`log(x) if c else warn(x)`), `producesUnusedValue` returns true unconditionally, flagging the statement — even though these are the idiomatic short-circuit / conditional-dispatch forms whose value is legitimately discarded. The rule is inconsistent: `doThing()` is fine, `c and doThing()` is not.
- failure-scenario:
  ```
  c and doThing()               # warns "result of expression is unused"
  log(x) if c else warn(x)      # warns "result of expression is unused"
  ```
  where doThing/log/warn are None-returning (side-effecting). A bare `doThing()` is not warned, so the wrapping is the only difference.
- proposed-test: test_warnings.cpp — assert `c and f()` and `f() if c else g()` (f/g None-returning locals) do NOT warn unused-result; keep flagging `a + b`, bare `x`, and a value-returning-local call.
- proposed-fix: for Logical/Conditional, recurse into the value-producing operands (the two branches / RHS) and flag only when those would themselves be flagged (i.e. treat a side-effecting call leaf as "used"). Or, more simply, exempt Logical/Conditional whose relevant leaves are CallExprs.
- confidence: high (reproduced)
