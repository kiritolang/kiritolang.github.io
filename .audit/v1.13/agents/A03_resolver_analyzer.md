# A03 — Resolver + Static Analyzer + Free-Variable Analysis

Audit of `src/kirito/resolver.hpp`, `src/kirito/analyzer.hpp`, `src/kirito/locals.hpp`.
Scope: name resolution, `name 'X' is not defined` compile errors, analyzer warnings and
their exemptions, free-variable analysis driving slot-locals.

Cross-checked against: `ast.hpp` (node set + `walkAssignTarget`), `parser.hpp` (nesting bound
`kMaxParseDepth`), `compiler.hpp` (`assignLocalSlots`/`defineSlot`), `bytecode_vm.hpp`
(`LoadLocal`/`StoreLocal`/`MakeFunction`), `runtime.hpp` (call binder + default evaluation),
`main.cpp` (warning emission), and the tests `test_warnings.cpp`, `test_slot_locals.cpp`,
`tools/tests/errors/surf_name_*.ki` / `surf_cls_*.ki`, `tools/tests/scripts/r7_language.ki`.

Static reasoning only (no build/run). Confirmed bugs first, then coverage gaps, then DRY/weak spots.

---

## CONFIRMED BUGS

### A03-1: Enclosing-function local referenced only by a NESTED function's parameter default is wrongly slotted → closure cannot see it (silent wrong value or NameError)
- **severity**: high
- **location**: `src/kirito/locals.hpp` `detail::CaptureScan::enterFunction` (lines 71-77, the
  default scan `scanExpr(*p.defaultValue, inNested, nb)` at line 72); interacts with
  `compiler.hpp::assignLocalSlots` (line 114) and `runtime.hpp` default evaluation (lines 2029-2031).
- **category**: correctness / capture analysis (slot-vs-name selection)
- **description**: `capturedLocals` decides which of a function F's locals must stay name-based
  (captured) vs. can be frame-slotted. When the scan enters a nested function literal `g`, it scans
  `g`'s **body** with `inNested=true` (correct) but scans `g`'s **parameter defaults** with the
  *incoming* `inNested` flag — which is `false` when `g` sits directly in F's body. A default
  expression, however, is compiled to its own Proto and **evaluated lazily at g's call time through
  g's closure** (`runtime.hpp:2031` `runBytecodeExpr(vm, scope, *default)`, where `scope`'s parent is
  `closure_` = F's scope). So a reference in g's default to one of F's locals is a genuine **capture**,
  exactly like a body reference, yet the scan classifies it as non-capture. That F-local then gets a
  frame slot (`assignLocalSlots` slots any name not in `captured` and not a param). `StoreLocal` writes
  **only** the frame slot, never `vars_` (`bytecode_vm.hpp:108`), and `MakeFunction` captures the raw
  scope handle with no spill (`bytecode_vm.hpp:158-162`). Result: at g's call the default's lookup of
  that name walks the closure's `vars_`, does **not** find the slotted local, and either (a) resolves
  a same-named binding in an enclosing scope → **silent wrong value**, or (b) throws
  `name 'X' is not defined` at runtime for valid code. The bug is narrow: it triggers only when the
  local is referenced *solely* in the nested default (a body reference would mark it captured and fix
  it), and only for a function nested *directly* in F (two-levels-deep already runs with `inNested=true`
  — see `test_slot_locals.cpp:41-42` analogue). By contrast a **class base** referenced with the same
  incoming flag is correct, because a base is compiled inline into F's body and evaluated in F's live
  frame (LoadLocal reaches the slot) — the divergence is specific to defaults' closure-deferred eval.
- **failure-scenario** (silent wrong answer — worst form):
  ```
  var z = 100
  var make = Function():
      var z = 10
      var g = Function(x, y = z): return x + y   # y default must see F's z = 10
      return g
  print(make()(5))   # expected 15; bug yields 105 (default resolves module z = 100)
  ```
  NameError form (no shadowing name in an outer scope):
  ```
  var make = Function():
      var z = 10
      var g = Function(x, y = z): return x + y
      return g
  print(make()(5))   # bug: "name 'z' is not defined" at call time
  ```
- **proposed-test**: add both scenarios to `test_slot_locals.cpp` (assert `== "15"`, and that the
  NameError form does NOT throw and equals `15`). Also a `tools/tests/scripts` golden covering the
  silent-shadowing case so a regression surfaces as a wrong value, not just a throw.
- **proposed-fix**: in `CaptureScan::enterFunction`, scan the nested function's defaults with
  `inNested=true` (they belong to the nested function's lazily-evaluated closure). Use an `nb` that
  includes the nested function's already-seen (earlier) params so `Function(a, b=a)` still treats `a`
  as the nested param, not a capture — i.e. scan default[i] with `inNested=true` and an `nb` grown
  incrementally over params[0..i-1]. (Over-approximating toward capture is always safe.)
- **confidence**: high (static data-flow is unambiguous; only unverified by an actual run).

### A03-2: `var` re-declaration resets the `used` flag → false "assigned but never used" for a variable used before a later (possibly conditional) re-declaration
- **severity**: medium
- **location**: `src/kirito/analyzer.hpp::declare` (lines 74-76, unconditional
  `scopes_.back().decls[name] = Decl{...false...}`); manifested via `analyzeStmt` VarDecl (line 146)
  and `popScope` (lines 65-71).
- **category**: analyzer false positive
- **description**: `declare` overwrites the per-name `Decl` with `used=false`, discarding any prior
  use. Because sub-blocks (`if`/`while`/`for`/…) share the enclosing function scope (no `pushScope`),
  a `var x` inside a nested block overwrites the earlier `var x`'s decl in the *same* `decls` map.
  If `x` was read before that re-declaration but not after, the reset `used=false` survives to
  `popScope`, which reports "variable 'x' is assigned but never used" — even though the source clearly
  reads `x`. The nested-block form emits **only** the misleading unused warning (no "re-declared"
  warning, since `declaredHere` is per-block), so nothing hints at the real cause.
- **failure-scenario**:
  ```
  var f = Function(c):
      var x = 1
      print(x)        # x IS used here
      if c:
          var x = 99  # re-declare in a sub-block -> resets used=false, no re-declare warning
      return 0
  # spurious: "variable 'x' is assigned but never used" at the `if` line
  ```
- **proposed-test**: `test_warnings.cpp` — assert `!has(w, "never used")` for the snippet above; and a
  same-block variant `var x=1 / print(x) / var x=2` asserting the re-declared warning fires but not a
  bogus "never used" (or document/accept that one).
- **proposed-fix**: when re-declaring an existing name in the same scope, preserve `used` (OR the old
  `used` into the new `Decl`), or skip the reset entirely (membership scoping means it is one binding).
- **confidence**: high.

### A03-3: Unused-result false positive when a call target is shadowed by a same-named parameter / non-function local
- **severity**: low-medium
- **location**: `src/kirito/analyzer.hpp::lookupFunc` (lines 83-89) used by `producesUnusedValue`
  (lines 245-249).
- **category**: analyzer false positive
- **description**: `lookupFunc` searches only each scope's `funcs` map (direct `var f = Function(...)`
  bindings), ignoring `decls`. If an inner scope shadows an outer always-returns-value function with a
  parameter (or a non-function local) of the same name, `lookupFunc` skips the closer binding and
  returns the outer function, so `producesUnusedValue` reports the shadowing call's result as unused —
  even though the actual callee (the parameter) may legitimately return `None`.
- **failure-scenario**:
  ```
  var compute = Function() -> Integer: return 5
  var f = Function(compute):    # param shadows the outer function
      compute()                 # calls the param (unknown return) -> should NOT be flagged
  # spurious: "result of expression is unused ..."
  ```
- **proposed-test**: `test_warnings.cpp` — the snippet above asserts `!has(w, "result of expression is unused")`.
- **proposed-fix**: in `lookupFunc`, stop as soon as a scope contains the name in `decls` but not in
  `funcs` (a closer non-function binding shadows the outer function → return `nullptr`).
- **confidence**: medium-high.

---

## COVERAGE GAPS

### A03-4: No test for the nested-default-captures-enclosing-local path (the A03-1 blind spot)
- **severity**: medium (test gap enabling A03-1)
- **location**: `test_slot_locals.cpp` (defaults tests lines 50-53 use only literal defaults or
  reference the *same* function's params); `r7_language.ki:433` covers `b = a + 1` (earlier param) and
  `:437` covers a default referencing an unbound name. None covers a nested function whose default
  references an **enclosing function's local var**.
- **category**: coverage
- **proposed-test**: see A03-1 tests.
- **confidence**: high.

### A03-5: "assigned but never used" — no test that a variable captured/used ONLY inside a nested closure is exempt
- **severity**: low
- **location**: `analyzer.hpp` `markUsed` walks outward (lines 77-82); documented behavior "or an
  enclosing closure" (header comment lines 9). `test_warnings.cpp` never exercises it.
- **category**: coverage
- **failure-scenario if it regressed**: `var f = Function():\n var x = 5\n var g = Function(): return x\n return g\n`
  should NOT warn `x` unused; currently correct but untested.
- **proposed-test**: assert `!has(w, "never used")` for the above.
- **confidence**: high.

### A03-6: Unreachable-code rule tested only for `return` and `break`; `throw` and `continue` untested
- **severity**: low
- **location**: `analyzer.hpp::isTerminator` (lines 113-116); tests at `test_warnings.cpp:106-119`
  cover `return` and `break` only.
- **category**: coverage
- **proposed-test**: two cases — code after a bare `throw "x"`, and code after `continue` in a loop —
  each asserting `has(w, "unreachable code")`; plus a negative: an `if` that returns on both branches
  followed by code must NOT warn (pins the deliberate conservatism at lines 110-112 of analyzer).
- **confidence**: high.

### A03-7: Unused-result rule — bare `Name` / `Member` / `Tuple` positive cases and the `None`-literal exemption untested
- **severity**: low
- **location**: `analyzer.hpp::producesUnusedValue` (Name/Index/Member/Tuple at lines 227-232, literal
  `monostate` exemption at line 236). Tests cover arithmetic (`:58`), index (`:62`), and a local-func
  call (`:66`) but not a bare `Name`, a bare `Member` (`obj.attr`), a bare `Tuple` (`a, b`), nor that a
  bare `None` / bare string literal is (or is not) flagged.
- **category**: coverage
- **proposed-test**: bare `x` (Name), bare `obj.attr` (Member), bare `a, b` (Tuple) each warn; a bare
  `None` statement does NOT warn (guards the `monostate` branch). Note: a bare **string literal**
  (LiteralExpr, non-monostate) DOES warn per line 236 — pin whichever is intended (docstring-style bare
  strings would be flagged, which may be surprising — see A03-11).
- **confidence**: high.

### A03-8: Self-assignment — member/index targets deliberately NOT flagged; untested
- **severity**: low
- **location**: `analyzer.hpp` lines 147-153 (only bare `NameExpr == NameExpr`).
- **category**: coverage
- **proposed-test**: `a.b = a.b` and `a[0] = a[0]` assert `!has(w, "self-assignment")` (pins the
  intentional carve-out for side-effecting protocols).
- **confidence**: high.

### A03-9: Resolver leniency (membership) — positive resolution of `catch as`/`with as`/post-loop `for` vars, and star-target rebinds, untested at the resolver layer
- **severity**: low
- **location**: `resolver.hpp` + `locals.hpp::collectStmtDecls` (catch/with/for names). Error tests
  cover many undefined-name cases (`surf_name_01..07`, `surf_cls_05/09`) but no test asserts that a
  name bound by `catch String as e` / `with ctx as m` / a `for k in xs` loop is **visible after** its
  block (membership), nor that a plain `a, *b = xs` where `b` is unbound is rejected (rebind must
  resolve), nor that `a, *b = xs` where both pre-exist resolves.
- **category**: coverage
- **proposed-test**: e.g. `for k in [1]: pass\ndiscard k` runs clean (resolves); `a, *b = [1,2]` with
  no prior `var b` throws `not defined` (rebind path via `checkTarget`→`StarExpr`→`checkName`).
- **confidence**: medium (behavior appears correct; unverified).

### A03-10: Warning emission order is not sorted by source position, and the "unused variable" subset is emitted in `unordered_map` (non-deterministic) order
- **severity**: low
- **location**: `analyzer.hpp::popScope` iterates `sc.decls` (a `fum::unordered_map`) at lines 67-70;
  these warnings are appended at scope *close*, after later-line warnings; `formatWarnings`
  (lines 318-325) and `main.cpp:219` print `warnings_` in vector order with no sort.
- **category**: quality / determinism (potential test flakiness)
- **description**: For a function with multiple unused locals, the relative order of their warnings is
  hash-bucket-dependent (non-deterministic across runs/compilers), and all unused-var warnings appear
  after the block's other warnings rather than in line order. Current tests use membership (`has(...)`)
  so they don't break, but any future exact-output or ordered assertion (and human-facing output)
  would be affected. `todo`/`error`-message ordering in docs/golden output could drift.
- **proposed-test / fix**: sort the returned `warnings_` by `(span.line, span.col)` before emission
  (stable), and add a test asserting deterministic order for two unused vars.
- **confidence**: high (mechanism), low (user impact today).

---

## WEAK SPOTS

### A03-11: Bare string-literal statement is flagged as "unused result" (no docstring carve-out)
- **severity**: low
- **location**: `analyzer.hpp::producesUnusedValue` line 236 — every non-`monostate` `LiteralExpr`
  returns true, so a lone `"some text"` statement warns.
- **category**: possible false positive / design
- **description**: A bare string used as a comment/docstring is flagged. This may be intentional
  (Kirito has `#` comments and no docstring concept), but it is undocumented and untested; worth an
  explicit decision + test either way.
- **confidence**: medium.

### A03-12: `-> Any` (and any non-`None` annotation) makes a bare call flagged even when it can return None
- **severity**: low
- **location**: `analyzer.hpp::alwaysReturnsValue` line 104 (`fn.returnAnnotation != "None"`).
- **category**: analyzer false positive (minor)
- **description**: A function annotated `-> Any` is treated as always value-producing, so a bare call
  to it is flagged as an unused result even though `Any` legitimately includes `None`. Rare, low-noise.
- **proposed-fix**: treat `Any` like an absent annotation (fall through to the last-statement check).
- **confidence**: medium.

### A03-13: Statement-level recursion in resolver/analyzer/locals has no independent depth guard — it relies entirely on the parser's `kMaxParseDepth`
- **severity**: low (informational)
- **location**: `resolver.hpp::checkExpr` guard `depth_ > 2800` (line 120); `analyzer.hpp::analyzeExpr`
  guard `depth_ > 2500` (line 258); `locals.hpp` has none. `parser.hpp` `kMaxParseDepth = 2000`
  (250 under sanitizers), and `parseIndentedSuite` counts block nesting toward it (parser.hpp:332).
- **category**: robustness / dead code
- **description**: Because the parser bounds *combined* expression+block+chain nesting at 2000 (250
  under ASan/TSan), the resolver's 2800 and analyzer's 2500 expression-depth guards can never fire —
  the AST that reaches them is already ≤2000 deep. So those guards are effectively dead defensive
  code (harmless), and `locals.hpp`'s (unguarded) `collectBlockDecls`/`CaptureScan` are safe only by
  the same external invariant. This is fine today, but the guards give a false sense of local
  self-protection: if `kMaxParseDepth` were ever raised above 2500/2800, the analyzer would silently
  stop descending (missing warnings) while the resolver's statement recursion (which has *no* counter)
  would be the first to overflow. Recommend either lowering the guards below `kMaxParseDepth` to make
  them real, or documenting the dependency and dropping the dead thresholds.
- **confidence**: high.

### A03-14: `popScope` unused-name check indexes `name[0]` — benign for non-empty identifiers, but assumes the parser never yields an empty name
- **severity**: info
- **location**: `analyzer.hpp:69` (`name[0] != '_'`).
- **category**: defensive
- **description**: `std::string::operator[](0)` on an empty string returns the null terminator (valid,
  not UB), so an empty name would simply be treated as non-underscore and warned. The parser does not
  produce empty identifiers, so this is not exploitable — noted only for completeness.
- **confidence**: high.

---

## DRY

### A03-15: The full expression-walk and statement-walk are hand-duplicated across three files (resolver, analyzer, locals) — a new AST node must be added in ≥3 (really ≥4) places or it is silently skipped
- **severity**: low-medium (maintenance / latent soundness)
- **location**: `resolver.hpp::checkExpr`/`checkStmt`, `analyzer.hpp::analyzeExpr`/`analyzeStmt`,
  `locals.hpp::detail::CaptureScan::scanExpr`/`scanStmt` (+ `collectStmtDecls`). Only
  `ast::walkAssignTarget` (ast.hpp:411) and `collectBlockDecls` are genuinely shared.
- **category**: DRY
- **description**: These are near-identical `dynamic_cast` ladders over all 17 `Expr` and 18 `Stmt`
  subclasses. I verified they are **currently in sync** (all three expr walks handle Name/Unary/Binary/
  Logical/Conditional/Call/Member/Index/Slice/List/Set/Dict/FString/Tuple/Star/Function; Literal = no-op;
  the stmt walks cover every binding/reference form and no-op on Break/Continue/Pass[/Todo]). But the
  duplication means a future node type (e.g. a comprehension, listed as future work in CLAUDE.md) that
  is added to only one or two of the walks will **silently escape** name resolution, use-tracking, or
  capture analysis — the last of which (A03-1-style) is a correctness bug, not just a missed warning.
  Consider a single generic AST visitor (a base walker invoking overridable per-node hooks) that all
  three passes derive from, so adding a node forces a compile-time update in one place. This mirrors
  the intent of the shared `walkAssignTarget`/`collectBlockDecls` but is not applied to the main walks.
- **proposed-test**: not directly testable; the mitigation is structural. A weaker guard: a unit test
  that constructs each AST node kind and asserts the resolver/analyzer/capture pass touches it.
- **confidence**: high (duplication is real; "silently skipped" is the concrete risk).

---

## Summary

- **Confirmed bugs**: 3 (A03-1 high, A03-2 medium, A03-3 low-medium).
- **Coverage gaps**: 7 (A03-4..A03-10).
- **Weak spots**: 4 (A03-11..A03-14).
- **DRY**: 1 (A03-15).

Status: COMPLETE.
