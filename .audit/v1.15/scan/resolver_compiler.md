# v1.15 audit — resolver / analyzer / compiler / locals / bytecode

Subsystem: name resolver, static analyzer, bytecode compiler.
Files: src/kirito/resolver.hpp, analyzer.hpp, compiler.hpp, locals.hpp, bytecode.hpp
Probe: ./build-debug/ki

## SUMMARY
Audited deeply and probed with ~25 adversarial scripts covering scope resolution, closures/capture,
slotted locals, all analyzer warnings, switch dispatch (literal/negative/float/-0.0/huge/dup), and
try/finally/with control-flow crossing (break/continue/return through cleanups & loops). This
subsystem is **exceptionally robust** — NO confirmed correctness/crash/memory bugs found. One minor
UX observation (out of subsystem, runtime type check) noted as LOW/SUSPECT.

## LOG (examined & ruled out)
- Resolver membership resolution: forward refs, recursion, mutual recursion, class-name binding,
  sibling-scope isolation, bare-`=` rebind-of-undefined, for-body var used after loop — ALL correct.
  Undefined names correctly rejected at compile time (u1..u4).
- Resolver param-default scope (earlier params visible, enclosing forward refs) — correct.
- Resolver depth guard (2800) & compiler depth guard (3000): both above the parser's own nesting
  bound (parser rejects ~2000-deep first with "expression nested too deeply"), so they are pure
  backstops — deep-nest probes (3500 parens / 4000 binops) throw cleanly, no crash/UB.
- locals.hpp CaptureScan: every Expr/Stmt kind traversed; params/with/catch/for names correctly
  shadow via `inner`/`nb`; nested class base scanned in enclosing scope, method bodies at inNested.
  Closure probes (counter closures, for-body `var j` captured, getter over mutated list) all correct
  — no captured local wrongly slotted, no stale-slot reads.
- Slot fallback: `var x = x + 1` reads outer via LoadLocal→LoadName fallback (t16, by design);
  read-before-assign with no outer → clean runtime "name not defined" (t15), no crash.
- Analyzer warnings: unused-var (incl. closure-write-only), unreachable, self-assign, re-declare,
  duplicate-param, todo, unused-result, discard-suppression — ALL fire correctly (warns.ki); no
  false positives on if/else re-decl, loop vars, params.
- Compiler assignment targets: multi-name, index/member, starred (first/mid/last), single `var *x`,
  mixed index+star — all correct; nested-tuple / bare-star / slice targets cleanly rejected with
  "invalid assignment target" (no crash), consistent with walkAssignTarget's Name/Index/Member/
  Tuple/Star coverage (slice hits default→handled downstream).
- Switch: literal O(1) dispatch, negative-literal folding, float-exact keys, -0.0/0.0 collapse,
  int≠float (case 1 ≠ case 1.0), duplicate detection (incl. -0.0 vs 0.0 and `-1` twice), 400-case
  table with subject evaluated exactly once, non-literal SwitchMatch chain — all correct.
  Verified compiler `constSwitchKey`/`literalSwitchKey` (floatToRoundtrip, prefixes N/B/I/F/S) matches
  runtime `scalarSwitchKey` (runtime.hpp:2343) byte-for-byte — no drift.
- Constant dedup (scalarConstKey): per-type prefixes make cross-type collision impossible; floats
  keyed on exact bits (bit_cast) so ±0.0/NaN never collapse; discarded dup handles are harmless.
- Control flow inline cleanup: return-in-finally overrides, break/continue in finally targeting outer
  loop, return crossing with/try (value parked in $ret slot), deeply nested try→for→with→try returns,
  break crossing with inside loop — ALL produce correct cleanup ordering (t1..t5, t20, t21).
- Eager resolution vs lazy compilation: resolver/analyzer descend all nested bodies eagerly; compiler
  compiles nested bodies lazily on first call — consistent, uncalled nested funcs still validated.

## FINDINGS

### F1 [LOW/SUSPECT] Undefined type annotation reports "must be <Name>" instead of "not defined"
- where: NOT in this subsystem — runtime type-annotation check (runtime/call binder). Noted for
  completeness only; the resolver/compiler deliberately do NOT validate type-annotation names
  (annotations are runtime type-name strings, resolved at call time), which is by-design.
- repro: `var f = Function(x : Bogus): return x` then `f(1)`
- actual: `error: argument 'x' must be Bogus, got Integer` (a typo'd/undefined annotation type
  silently rejects EVERY argument rather than reporting the type name is unknown).
- expected: arguably a clearer "type 'Bogus' is not defined" at the annotation site.
- fix idea: the runtime type check (not resolver) could verify the annotation name resolves to a
  type before comparing. Belongs to the runtime/call-binder subsystem, not resolver/compiler. Left
  as SUSPECT for the orchestrator to route.

## CONCLUSION
No actionable defects in resolver/analyzer/compiler/locals/bytecode. Subsystem is solid.
