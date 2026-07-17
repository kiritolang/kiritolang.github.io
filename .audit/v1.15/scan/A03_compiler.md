# A03 Compiler/Resolver

Round v1.15 (target patch 1.15.1). Scope: `src/kirito/compiler.hpp`, `src/kirito/bytecode.hpp`,
`src/kirito/resolver.hpp`, `src/kirito/analyzer.hpp`, `src/kirito/locals.hpp`. Read-only audit +
empirical checks against `./build-debug/ki`.

Status: COMPLETE.

### Severity counts
- HIGH: 0
- MED: 0
- LOW: 2
- INFO: 1
- Coverage gaps: 1 (bundled finding, several concrete cases)

Context: this round's headline new subsystem touching my files is the **function/class
self-serialization** feature (commit `9f0674a`), which added `freeVariables`/`eagerFreeVariables` to
`locals.hpp` (the dual of the existing `capturedLocals`). Prior rounds (v1.13, v1.14, v1.14.1) already
gave the resolver/analyzer/locals slot-layout machinery a clean bill of health (see those rounds'
`agents/A03_*.md` for what was already checked and found correct — not re-litigated here unless a
regression is found). This round's scan concentrates extra scrutiny on the new free-variable scan,
plus a fresh adversarial pass (nested closures, float const dedup, switch, self-shadowing) per the
brief.


## Empirical verification of the new free-variable scan (locals.hpp: `freeVariables`/`eagerFreeVariables`)

Commit `9f0674a` ("functions and classes serialize by default") added `detail::FreeVarScan` plus
`freeVariables(FunctionExpr)`, `freeVariables(ClassStmt)`, `eagerFreeVariables(ClassStmt)` to
`locals.hpp` — the dual of the pre-existing `capturedLocals`/`CaptureScan`, and the highest-risk new
surface in my subsystem this round (it is the compile-time analysis that tells `stdlib_serde.hpp`
which enclosing-scope names a serialized `Function`/`class` must carry). I read it line-by-line
against `CaptureScan` and then drove it empirically through `dump`/`serialize` round-trips with
`./build-debug/ki` (scripts under the scratchpad; not checked in, per read-only-on-tests):

- 3-level nested closure (`make(a) -> mid() -> inner()`, free var `a`/`b`/`c` chained through two
  `MakeFunction` hops) — round-trips and evaluates correctly after `dump.loads(dump.dumps(f))`.
- `switch` inside a closure, case values integer-literal, body referencing the outer captured `x` —
  correct after round-trip (105/205/5 for inputs 1/2/3).
- `try/throw/catch as e` inside a closure, with an OUTER module-level variable also named `e` — the
  catch-bound `e` correctly shadows the outer `e` (returns "boom", not the outer string), confirming
  `FreeVarScan::stmt`'s `TryStmt` case does not spuriously free-var a name that is really the handler's
  own catch-binding (this works because `collectBlockDecls` already includes catch names in the
  enclosing function scope's binding set before the scan starts — try/catch shares the function scope,
  it doesn't introduce one — so `bound` already contains `e` throughout, matching `CaptureScan`).
- Top-level mutual recursion (`isEven`/`isOdd`) serialized independently — round-trips and both still
  call each other correctly (dual of the existing `capturedLocals` mutual-recursion check, now via the
  free-var path).
- Class inheritance + `_super_` — a `Dog(Animal)` instance serializes/deserializes and
  `self._super_().speak()` still resolves after rebuild.
- **Self-recursive class reference through a non-`_init_` method is NOT itself exercised by the
  existing golden test** (`spec_serialize_functions.ki` only exercises self-recursion via `_init_`
  building a chain, and mutual recursion via *top-level functions*, not classes) — I additionally
  verified a class whose `_init_` recursively instantiates itself (`Node`, depth-capped
  self-referential tree) serializes/deserializes and the rebuilt instance is a genuine deep copy (post
  deserialize mutation of the original does not perturb the clone) — correct. This confirms
  `freeVariables(ClassStmt)`'s design choice — the class's OWN name is deliberately excluded from
  `bound` inside `klass()` (a class's name binds in its *enclosing* scope, not its own body) so a
  self-reference is correctly captured as a free variable for the final wiring pass to bind back to the
  rebuilt class itself.
- A parameter default referencing an **enclosing function's local** (`Function(x, y = a): ...` nested
  inside `Function(a): ...`) is correctly free-var'd and survives the round trip (101 / 3 for
  `f2(1)` / `f2(1,2)`) — `FreeVarScan::func` scans each default with only the *earlier* params bound,
  matching `CaptureScan::enterFunction`'s identical left-to-right rule (A03 finding from v1.14.1,
  still correct, now shared by both scanners).
- Deep closure-capture chain (24 nested `Function()` levels, innermost returning an outer `var`
  captured through all 24 `MakeFunction`/env-hop levels) evaluates correctly (`42`) — confirms the
  strict-lexical-addressing `(depth, index)` walk is sound at nesting depths well past anything a
  realistic program would use.
- Exact-bit float constant dedup (`scalarConstKey`, compiler.hpp:74-85): confirmed `0.0` and `-0.0`
  are NOT deduped into one `consts` slot (their `bit_cast<uint64_t>` keys differ), while the SEPARATE
  switch-case key normalization (`literalSwitchKey`/`constSwitchKey`, compiler.hpp:177-210, which
  intentionally folds `-0.0`→`0.0`) correctly makes `case 0.0` / `case -0.0` collide as a **duplicate
  switch case** — verified by triggering the actual "duplicate switch case value" runtime error. This
  is deliberate and matches the language's `0.0 == -0.0` equality rule; not a bug (the two keying
  schemes intentionally differ for their two different purposes — `consts` dedup must NOT conflate
  distinct bit patterns since a switch table only needs `==`-consistent keys, an ordinary constant slot
  should not silently collapse a value someone might reasonably assume is bit-distinguishable, e.g. via
  `1.0/x`).

No bug found in any of the above — the new free-variable scan is correct and consistent with the
pre-existing `capturedLocals`, and the strict-lexical-addressing contract (resolver `(depth,index)`
annotation == compiler/runtime slot layout) continues to hold under adversarial nesting. This matches
and extends the "no functional bug" verdict of the v1.14.1 A03 round for the pre-existing machinery.

---

### A03-1: `FreeVarScan` is a FOURTH hand-rolled, non-exhaustive-by-construction AST traversal (aggravates the existing visitor-bypass hazard)

**Severity:** LOW (latent maintenance hazard; not currently triggerable — see verification above).
**Confidence:** confirmed.
**Location:** locals.hpp:203-292 (`detail::FreeVarScan::expr`/`stmt`), alongside the pre-existing
resolver.hpp:159-186/225-268 (`checkStmt`/`checkExpr`), analyzer.hpp:138-205/254-300
(`analyzeStmt`/`analyzeExpr`), and locals.hpp:112-179 (`CaptureScan::scanExpr`/`scanStmt`).

**What.** v1.14.1's A03-1 already flagged that the resolver, analyzer, and `CaptureScan` each
hand-roll their own `dynamic_cast` chain over every `Expr`/`Stmt` subtype instead of routing through
`ast::ExprVisitor`/`StmtVisitor` (the mechanism the *compiler* uses, which makes "handle every node" a
COMPILE ERROR for a new AST node). This round's new `FreeVarScan` (`freeVariables`/
`eagerFreeVariables`) is a **fourth** such hand-rolled traversal, with the exact same shape (30+
`dynamic_cast`s duplicating the same `Expr`/`Stmt` case list a fourth time). It is presently complete
and correct (verified above, and cross-checked node-for-node against `CaptureScan`'s case list — the
two are exhaustively identical modulo the different name-membership test in the `NameExpr` case), but
a future AST node (CLAUDE.md lists comprehensions as a planned "not yet done" feature, and a
comprehension binds names in a nested scope — exactly the scope-sensitive case both `CaptureScan` and
`FreeVarScan` care about) would silently fall through the trailing `else`/no-op in FreeVarScan with NO
compile error, under-reporting a function's free variables. The consequence for THIS scanner specifically
(distinct from the resolver/analyzer consequences already documented in v1.14.1's A03-1) is novel and
somewhat worse than a stale lint: a name silently missing from `freeVariables()`'s result means
`stdlib_serde.hpp` never snapshots that captured value, so a serialized function/class that uses the
missed construct would deserialize into a closure missing a free binding — most likely surfacing as a
run-time `name 'X' is not defined` inside the rebuilt closure (or, worse, silently reading a
coincidentally-same-named global/builtin instead) rather than a clean compile-time or serialize-time
error.

**Repro.** Not triggerable today (no such AST node exists yet); this is a forward-looking maintenance
finding, same class as v1.14.1 A03-1, now quadrupled instead of tripled.

**Proposed fix.** Same as the outstanding v1.14.1 A03-1 recommendation: introduce one shared
"visit-children" traversal (either via `ExprVisitor`/`StmtVisitor` proper, or a single exhaustive
`switch (e.exprKind())`/`switch (s.stmtKind())` helper with a `static_assert`/`default: static_assert`
exhaustiveness guard) that `resolver.hpp`, `analyzer.hpp`, and BOTH `locals.hpp` scanners (`CaptureScan`
and `FreeVarScan`) call, so a new node type becomes a compile error in all four passes at once instead
of a silent skip in each independently. Given there are now four independent copies of the same
traversal shape, the DRY case for doing this is stronger than it was in v1.14.1.

**Proposed test.** None possible today without adding the hypothetical new node; the actionable step is
the refactor itself, which converts "silently skip" into "fails to compile" — a structural guarantee,
not a test case.

---

### A03-2: Coverage gap — the new free-variable scan has no direct C++/`.ki` regression pinning its adversarial scope shapes

**Severity:** N/A (coverage gap). **Confidence:** confirmed (verified by grep + reading the existing
golden test).

**What.** `tools/tests/scripts/spec_serialize_functions.ki` (the only test exercising
`freeVariables`/`eagerFreeVariables`, indirectly through `stdlib_serde.hpp`) covers: a plain function,
a single-level closure, self-recursion via a top-level `var`, mutual recursion via two top-level
`var`s, a helper function referenced by another, a stdlib-module reference, a nested-closure-returning
function, shared identity through a container, and class value/instance/inheritance round-trips. It
does NOT cover (all of which I hand-verified correct above, empirically, but none are pinned as
regression tests):
- a closure whose free variable is used only inside a `switch` case body;
- a closure containing `try`/`catch as NAME` where `NAME` shadows an outer-scope name of the same
  identifier (the exact case that would expose a `TryStmt`-handling divergence between `CaptureScan`
  and `FreeVarScan` if one existed);
- a **class** (not a function) whose method recursively instantiates its own class (self-reference
  captured as a free var, resolved back to itself by the rebuild's final wiring pass) — the existing
  test only exercises class self-reference via `_init_` for `Point`'s trivial ctor and inheritance, not
  a method building a `Self(...)`-shaped recursive structure;
- a parameter default that captures an ENCLOSING function's local (as opposed to an earlier sibling
  parameter, which IS implicitly covered via the resolver's own tests) through the serialization path
  specifically;
- deep closure-capture chains (3+ levels) through `dump`/`serialize` specifically (only a single-level
  "nested closure" case — `makeAdder`/`add10` — currently exists).

**Proposed fix:** N/A — findings, not source changes.

**Proposed test:** Add cases for each bullet above to `spec_serialize_functions.ki` (or a companion
golden script), each asserting the round-tripped closure/instance still produces the correct value —
this is the regression net that would catch a future A03-1-style silent-skip regression in
`FreeVarScan` specifically, since none of the existing checks would fail if e.g. the `TryStmt` or
`SwitchStmt` case were accidentally dropped from `FreeVarScan::stmt`.

---

### A03-3 (INFO): `capturedLocals`/`CaptureScan` and the new `freeVariables`/`FreeVarScan` have no recursion-depth guard, unlike resolver/analyzer/compiler — but this is provably non-triggerable

**Severity:** INFO. **Confidence:** confirmed (empirically bounded).

**What.** `resolver.hpp` (`checkExpr`, depth cap 2800), `analyzer.hpp` (`analyzeExpr`, cap 2500), and
`compiler.hpp` (`DepthScope`, `kMaxDepth = 3000`) all explicitly bound their own recursive AST descent
against a pathologically deep tree, matching the language-wide guarantee that "deeply nested
source/data structures throw instead of overflowing the native stack." `locals.hpp`'s
`CaptureScan::scanExpr`/`scanStmt` (pre-existing) and the new `FreeVarScan::expr`/`stmt` have **no**
such guard — an unbounded recursive descent with per-frame `NameSet` copies (`NameSet inner = nb;`/
`NameSet inner = outer;`, which for `fum::unordered_set` are non-trivial-sized stack objects per level).

**Why this is not currently a bug.** All AST nesting, of every kind (expression nesting AND
`Function():`-body nesting), is gated at PARSE time by `parser.hpp`'s `kMaxParseDepth` (250 under
sanitizer builds, 2000 otherwise — parser.hpp:446/448), well below the resolver/analyzer/compiler's own
caps. I empirically confirmed this bound is the true ceiling by constructing a 4000-level chain of
nested `Function(): ...` definitions (which is exactly the shape that would make `capturedLocals`'
`CaptureScan::enterFunction` recurse arbitrarily deep, since it must descend through every nested
function body to find cross-level captures): it is rejected at parse time with `"expression nested too
deeply"` around depth ~1000, long before `capturedLocals`/`freeVariables` (called only on an
already-successfully-parsed AST) ever gets a chance to run on it. So this hazard is real in isolation
but dominated/masked by an earlier, unconditional guard — the same "safe in practice, no shared
derivation/cross-reference comment" situation the v1.14.1 A03-4 finding already accepted for the
resolver-vs-analyzer depth-cap mismatch. Recorded for completeness; no fix proposed (adding a guard
here would be pure defense-in-depth with no reachable trigger under the current parser bound).

---

## Things checked and found CORRECT (no finding, beyond what v1.13/v1.14/v1.14.1 already validated)

- Slot/env-index layout agreement by construction: `resolver::computeFunctionEnvIndex` /
  `computeClassEnvIndex` and `compiler::assignLocalSlots` / `collectClassEnvSlots` all derive from the
  same `collectBlockDeclsOrdered` + `capturedLocals` filter (locals.hpp), and the runtime
  (`bytecode_vm.hpp::run`, `KiFunction::callFull` in runtime.hpp) defines ALL parameters (captured and
  non-captured alike) into the scope env at their absolute positional index, then appends captured
  non-param locals starting at `params.size()` — matching the resolver's `next = fn.params.size()`
  starting offset exactly. Traced through both the fast-bindable and general-bind paths in
  `KiFunction::callFull` (runtime.hpp:2035-2127); both define every param by name into `env` before the
  bytecode frame's own `paramSlots` placement, so a captured param's env index is always its absolute
  original position regardless of how many sibling params are non-captured.
- `freeVariables`/`eagerFreeVariables` exhaustively match `capturedLocals`'s `Expr`/`Stmt` case
  coverage (compared node-for-node); no missing case in either relative to the other, and none relative
  to the resolver/analyzer's own case lists.
- Float constant dedup (`0.0`/`-0.0`/NaN) and switch-case key folding are correct and intentionally
  divergent for their two different jobs — reconfirmed empirically (see above), not a re-flag of the
  v1.13/v1.14 "by design" ledger entries (this is a distinct code path — `scalarConstKey` vs.
  `literalSwitchKey`/`constSwitchKey` — that happens to share the exact-bits vs. IEEE-`==`-consistent
  design tension already accepted elsewhere in the codebase).
- Deep closure nesting (24 levels) and mutual/self recursion (functions and classes) all resolve and
  execute correctly both natively and after a `dump`/`serialize` round-trip.


## Summary

Scope: `compiler.hpp`, `bytecode.hpp`, `resolver.hpp`, `analyzer.hpp`, `locals.hpp`. No HIGH or MED
findings. The subsystem's pre-existing slot/env-layout machinery (already given a clean bill of health
in v1.13/v1.14/v1.14.1) continues to hold under fresh adversarial pressure (deep nesting, float
0.0/-0.0 constant dedup vs. switch-key folding, self/mutual recursion). The round's new surface in this
subsystem — `locals.hpp`'s `freeVariables`/`eagerFreeVariables` (added for function/class
self-serialization, commit `9f0674a`) — was read against its sibling `capturedLocals`/`CaptureScan`
and driven through concrete `dump`/`serialize` round-trips (nested closures, switch-in-closure,
try/catch name-shadowing, self-recursive classes, param defaults capturing an enclosing local, 24-level
closure chains): no bug found.

Findings: 2 LOW + 1 INFO + 1 coverage-gap bundle, all forward-looking/hardening rather than active
bugs — (1) the new `FreeVarScan` is a fourth hand-rolled AST traversal bypassing `ExprVisitor`/
`StmtVisitor`, aggravating the existing (accepted, v1.14.1) visitor-bypass maintenance hazard; (2) the
new serialization free-var path lacks direct regression coverage for several scope shapes I verified
manually (switch/try-catch-shadow/self-recursive-class/deep-chain — all correct today, none pinned);
(3) `capturedLocals`/`freeVariables` have no recursion-depth guard, but this is provably non-
triggerable because the parser's own nesting bound (~1000-2000) gates all AST depth first (empirically
confirmed with a 4000-level nested-function-definition input, rejected at parse time).
