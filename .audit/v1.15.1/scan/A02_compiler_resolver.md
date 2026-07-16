# A02 — compiler + resolver + analyzer (v1.15.1)

Scope: `src/kirito/compiler.hpp`, `src/kirito/resolver.hpp`, `src/kirito/analyzer.hpp`,
`src/kirito/bytecode.hpp`, `src/kirito/locals.hpp`.

Binary probed: `./build-debug/ki` (1.15.0, debug preset — so the debug-only slot-name assertion is live).

Status: DONE — 3 findings (1 HIGH, 1 MED, 1 LOW). See Coverage gaps + Non-findings at the end.

## Findings

### A02-1: compiler-generated hidden locals (`$with0`/`$exc0`) leak into a module's public exports  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/compiler.hpp:155-157` (`ensureHiddenSlot`), `:571-573` (`$with`), `:488-489` (`$exc`);
  export filters `src/kirito/runtime.hpp:2449` and `:2546`.
- What: `ensureHiddenSlot` only allocates a frame slot when `slotsEnabled_` — i.e. inside a **true
  function body**. At module/class scope `slotsEnabled_` is false, so `emitStore("$with0", ...)` falls
  through to `Op::StoreName`, which **defines `$with0` in the module scope's EnvValue**. Neither module
  export filter excludes `$`-prefixed names, so the compiler's internal temporary becomes a **public
  member of the module**. The compiler's own comment ("`$` can't appear in a user name") guarantees no
  *collision*, but nothing keeps the binding private.
  Two consequences: (a) `inspect(module)` prints an implementation detail; (b) the `$with0` binding
  holds a **live reference to the context manager for the module's whole lifetime** — a top-level
  `with io.open(...) as f:` retains the `File` object forever, defeating the point of the `with`.
- Repro:
  ```sh
  # /tmp/a02/modfile.ki
  var io = import("io")
  with io.open("/tmp/a02/data.txt", "w") as f:
      f.write("hi")
  var done = 1
  # /tmp/a02/usefile.ki
  var io = import("io")
  var m = import("modfile")
  io.print(inspect(m))
  ```
  ```
  $ ./build-debug/ki --lib /tmp/a02 usefile.ki
  module modfile:
    $with0: File          <-- compiler internal, retains the open File
    done: Integer
    f: File
    io: Module
  ```
  Same for `$exc0` on a top-level `try/finally` exception path:
  ```
  module modexc2:
    $exc0: String         <-- compiler internal
    e: String
    io: Module
    sink: List
  ```
- Impact: any `.ki` module (or the REPL, same indexed-module scope) with a top-level `with` or
  `try/finally`. Cosmetic on `inspect`; a genuine **resource-retention leak** for `with`-managed
  files/sockets held at module scope.
- Proposed fix: exclude `$`-prefixed names in both export filters — but better at the source: give the
  two filters ONE shared predicate (they already diverge, see A02-2) and have it drop
  `k.front() == '$'`. Cleanest of all would be for `ensureHiddenSlot` to allocate a hidden slot at
  module scope too, but module scopes are deliberately not slotted, so filtering is the low-risk fix.
  No documented contract at risk (`$` names are unwritable and unreadable from Kirito — the lexer
  rejects `$`, verified: `error: unexpected character '$'`).
- Proposed test: `tools/tests/scripts/` — a module with a top-level `with` + `try/finally`, imported,
  asserting `inspect(m)` contains neither `$with` nor `$exc`. Name it for the symptom, e.g.
  `r12_module_hidden_locals.ki`.

### A02-2: the two module-export filters diverge — a frozen module hides `_private` top-level names, a `.ki` module does not  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/runtime.hpp:2449` vs `:2546` (duplicated, non-identical export loops).
- What: the same conceptual step ("publish a module's top-level bindings as its members") is written
  twice with **different rules**. The frozen-source-module loader filters `k.front() != '_'`
  ("hide private top-level names"); the `.ki`-file loader does not. So an identical module body exports
  a different surface depending on how it is loaded. This is exactly the DRY hazard the round brief
  calls out — a fix applied to one copy and not the other.
- Repro:
  ```sh
  # /tmp/a02/modpriv.ki
  var _secret = 1234
  var public = 5
  ```
  ```
  $ ./build-debug/ki --lib /tmp/a02 usepriv.ki
  module modpriv:
    public: Integer      <-- inspect hides it (inspect does its own _ filter)
  1234                   <-- but m._secret IS readable
  ```
  A frozen module's `_name` is absent from `members` entirely, so the same access would throw.
- Impact: latent. I grepped `examples/`, `tools/tests/`, `kpm/` for a module-qualified `mod._name`
  read and found **none** (every `x._y` hit is instance-attribute access), so nothing depends on the
  `.ki` side today. But the inconsistency is a live trap for the next change.
- Proposed fix: single-source the loop into one helper (`publishModuleMembers(vm, scope, name)`) used
  by both call sites. Note the *semantic* question of which rule wins is a judgement call for the main
  agent: hiding `_` on `.ki` modules is a real behaviour change (low risk per the grep, but it is a
  change); adding `$` filtering (A02-1) is not. Recommend: unify the code, adopt the frozen rule
  (`_` and `$` hidden) — module-level `_private` is then consistent with the documented class-member
  privacy convention.
- Proposed test: assert both loaders agree — a `.ki` module and a frozen module with the same
  `_private` top-level name expose the same surface.


### A02-3: a duplicate parameter name desynchronizes the resolver's env layout from the runtime's — wrong-slot reads (debug: assertion abort; release: silent wrong binding)  [severity: HIGH] [confidence: confirmed]
- Location: `src/kirito/resolver.hpp:138-149` (`computeFunctionEnvIndex`); root cause interacts with
  `src/kirito/environment.hpp:45-51` (`EnvValue::define`) and `src/kirito/runtime.hpp:2090` / `:2128`
  (`KiFunction::callFull`, both bind paths).
- What: strict lexical addressing's core contract is that a name's `(depth, index)` is **correct by
  construction**, because the resolver's layout and the runtime's frame-entry pre-declaration share one
  deterministic order. That contract **breaks whenever a function has a duplicate parameter name** —
  which is only an analyzer **warning**, not an error, so it is ordinary reachable source.

  `computeFunctionEnvIndex` assumes the runtime scope env holds **one entry per parameter**:
  ```cpp
  for (std::size_t i = 0; i < fn.params.size(); ++i) {
      paramSet.insert(fn.params[i].name);
      if (captured.count(fn.params[i].name))
          s.envIndex.emplace(fn.params[i].name, static_cast<uint32_t>(i));   // (2) POSITIONAL index
  }
  uint32_t next = static_cast<uint32_t>(fn.params.size());                    // (1) WRONG BASE
  ```
  But `callFull` binds params with `env.define(...)` in a loop, and `EnvValue::define`
  **overwrites a duplicate in place rather than appending**:
  ```cpp
  for (auto& [k, v] : vars_)
      if (k == name) { v = h; return; }   // <-- dedup
  vars_.push_back(name, h);
  ```
  So the env actually holds one entry per **distinct** param name. Two independent desyncs follow:
  1. **base offset** — captured non-param locals are indexed from `params.size()`, but the runtime
     pre-declares `Proto::envSlots` starting at `distinct_params`. Off by (dups).
  2. **captured params after a duplicate** — indexed by *positional* `i`, but they live at their
     *deduped* position.

  Both bind paths in `callFull` (fast, line 2090; slow/defaults/keywords, line 2128) are affected.
- Repro (all with `./build-debug/ki`):
  ```kirito
  # dup2.ki -- duplicate param + TWO captured locals
  var io = import("io")
  var f = Function(a, a):
      var x = 100
      var y = 200
      var g = Function():
          return [x, y]
      return g()
  io.print(f(1, 2))
  ```
  ```
  $ ./build-debug/ki dup2.ki
  dup2.ki:2:9: warning: duplicate parameter name 'a'
  ki: src/kirito/bytecode_vm.hpp:114: kirito::Handle kirito::BytecodeVM::run(...):
      Assertion `e.nameAt(ref.index) == ref.name' failed.
  Aborted (core dumped)          <-- exit 134
  ```
  With only ONE captured local the index lands out of range and it degrades to a **spurious name
  error** for a variable that is plainly defined and assigned:
  ```
  $ ./build-debug/ki dup1.ki      # Function(a, a): var x = 100; var g = Function(): return x
  dup1.ki:2:9: warning: duplicate parameter name 'a'
  dup1.ki:5:16: error: name 'x' is not defined       <-- x IS defined
  ```
  Shape map (confirmed live):
  | shape | result |
  |---|---|
  | `Function(a, a)` + 2 captured locals | **assert abort** (d2) |
  | `Function(a, b, a)` + 2 captured locals | **assert abort** (d3) |
  | `Function(a, a, a)` + 3 captured locals | **assert abort** (d7) |
  | `Function(a, a, k = 9)` + 2 captured locals (slow bind path) | **assert abort** (d8) |
  | `Function(a, a, b, c)` + captured params b, c | **assert abort** (d10) |
  | `Function(a, a)` + 1 captured local | spurious `name 'x' is not defined` (d1) |
  | `Function(a, a, b)` + captured param b | spurious `name 'b' is not defined` (d9) |
  | `Function(a, a)` + no captured local | correct (d5) |
  | `Function(a, a)` + the duplicate itself captured | correct — `emplace` keeps index 0 (d6) |
  | `Function(a)` + a body `var a` (NOT a dup param) | correct (d4) |
- Impact: **release builds compile the assertion out** (`NDEBUG`), so the same programs do not abort —
  they read the **wrong binding silently**. The assertion firing is itself the proof of the wrong-slot
  condition: it asserts exactly `e.nameAt(ref.index) == ref.name`, so its failure means the slot the VM
  is about to read names a *different* variable. In `dup2.ki` a closure reading `x` would return `y`'s
  value. That is silent data corruption from source that only warns. (I could not execute a release
  build to demonstrate — the round forbids building — so the release consequence is stated as
  inference from the assert condition, not as an observed run.)
  Who hits it: anyone who typos a repeated parameter name in a function that also closes over a local —
  the warning is easy to miss in a noisy build, and the failure mode is a wrong value, not a crash.
- Proposed fix (low risk, preserves the documented warning-not-error contract): index by **distinct**
  param position in `computeFunctionEnvIndex`, mirroring `EnvValue::define`'s dedup:
  ```cpp
  void computeFunctionEnvIndex(Scope& s, const ast::FunctionExpr& fn) {
      NameSet captured = capturedLocals(fn.params, fn.body);
      NameSet paramSet;
      uint32_t next = 0;
      for (const auto& p : fn.params) {
          if (!paramSet.insert(p.name).second) continue;  // a duplicate shares the first one's env slot
          if (captured.count(p.name)) s.envIndex.emplace(p.name, next);
          ++next;
      }
      for (const auto& name : collectBlockDeclsOrdered(fn.body))
          if (captured.count(name) && !paramSet.count(name)) s.envIndex.emplace(name, next++);
  }
  ```
  This fixes both desyncs at once and is a no-op for every function without a duplicate param
  (`next` then equals `fn.params.size()` exactly, and each distinct param's index equals its position).
  Note `Compiler::assignLocalSlots` needs no change: `paramSlots` is indexed by param *position* and
  `setLocal` simply writes the slot twice (last wins, matching `define`'s last-wins), and `envSlots`
  order already matches.
  **Alternative worth considering** (bigger contract change, so main agent's call): reject a duplicate
  parameter name as a hard error at parse time, like Python's `SyntaxError: duplicate argument`. That
  removes the whole class rather than making a dubious `Function(a, a)` addressable. But CLAUDE.md
  documents duplicate params as one of the analyzer's **warnings**, so this would move a documented
  behaviour — the index fix does not.
- Proposed test: `tools/tests/unit/test_scoping*.cpp` or a golden `.ki` named for the symptom, e.g.
  `tools/tests/scripts/r12_dup_param_capture.ki`, asserting each row of the shape map above —
  crucially `Function(a, a)` + 2 captured locals returns `[100, 200]` (not `[200, ...]` / not an
  abort), and `Function(a, a, b)` + captured `b` returns b's real value. Must be verified to FAIL
  (abort/name-error) on the unfixed build — it does today, exit 134.

---

## Coverage gaps (most valuable first)

1. **A duplicate parameter name is never executed by any test.** The only coverage is
   `tools/tests/unit/test_warnings.cpp:131-139`, which asserts the *warning* text using
   `var f = Function(a, b, a):\n    return a\n` — a function that is **never called** and has **no
   captured locals**, i.e. precisely the one shape that cannot trigger A02-3. `grep -rnE
   "Function\(\s*([a-z_]+)\s*,\s*\1\s*[,)]"` over `unit/*.cpp scripts/*.ki errors/*.ki` returns
   **nothing** else. Propose: `tools/tests/scripts/r12_dup_param_capture.ki` covering the A02-3 shape
   map (dup + 1 captured local, dup + 2 captured locals, dup + captured param after it, dup via the
   slow bind path, and the three currently-correct control shapes).
2. **`$ret` parking — the v1.15 feature — has no test.** `Compiler::visit(ReturnStmt)`'s
   `crossesCleanup` branch (compiler.hpp:372-382) is the fix for A04-1 and is exercised by nothing in
   `test_slot_locals.cpp` / `test_bytecode.cpp`. Propose asserting: `return` crossing a `finally` that
   contains a `break`; `return` crossing nested finallys; `return` crossing a `with`. (All verified
   correct live — this is a pin-the-fix gap, not a bug.)
3. **`SaveExcSpan`/`RestoreExcSpan` + `Proto::excSpanSlots` have no test.** The contract (an outer
   in-flight exception's *span* must survive a finally body that itself handles an exception) is
   verified live here and works to 3 levels, but nothing pins it. Propose an `errors/`-suite entry:
   a nested try-in-finally whose `.experr` requires the OUTER throw's line number.
4. **Module-export surface is untested for compiler internals** — no test asserts that `inspect(m)` /
   `m.<name>` excludes `$`-prefixed names (A02-1), nor that the two loaders agree (A02-2).
5. **Switch: the O(1) table vs the `SwitchMatch` chain are never cross-checked.** Both paths exist and
   I verified live that they agree on `1` vs `1.0`, `0.0`/`-0.0`, `True`, `NaN`, and a miss — but no
   test drives the *same* case set through both (the chain is only reached via a non-literal case
   value). Propose a `.ki` test running a literal switch and a `var`-valued switch over an identical
   value set and asserting identical results.
6. **Class-body env indexing is thinly covered.** `test_resolver.cpp` has one bare-ref case
   (`class K:\n var n = 7\n var get = Function(self): return n`). Untested: a **forward** reference to
   a sibling method defined later in the body (works live), a class body inside a function capturing
   both an outer local and a sibling class-body name (works live).
7. **REPL slot stability across lines is only smoke-tested.** `test_resolver.cpp`'s REPL block checks
   `counter = counter + 1` doesn't throw. Untested: a closure defined on an earlier line **observing a
   later mutation** (the documented contract; works live), and that appending new names on later lines
   does not shift earlier slots.
8. **`test_bytecode.cpp` (135 lines) has no operand-stack-balance stress.** Deeply interleaved
   `for`/`with`/`try`/`finally`/`switch` with `break`/`continue`/`return` crossing them — my
   `stress.ki` probe is the shape; nothing equivalent is in the suite.
9. **`locals.hpp`'s `freeVariables`/`eagerFreeVariables` have no direct unit test** — they are covered
   only indirectly through serde round-trips.

## Non-findings (probed, correct — with the inputs used)

- **Strict lexical addressing, core contracts** — all correct: `var sorted = sorted(xs)` is
  `name 'sorted' is not defined` (not the builtin); read-before-write is `name 'y' is not defined`
  and does **not** walk to an outer/builtin binding (probed with an outer `var y = 99` present);
  closures capturing params + locals; nested closures; `mk(1000)`/`mk(2000)` instances stay independent.
- **Parameter defaults**: `Function(a, b = a + 1, c = b * 2)` → `f(10)`=`[10,11,22]`, `f(10,20)`=
  `[10,20,40]`, `f(a=1, c=9)`=`[1,2,9]`. Defaults run as their own Proto and correctly see earlier params.
- **REPL persistent scope**: a closure from line 3 observes a mutation from line 5 (`1000`→`2000`), and
  still reads correctly after a *new* name is appended on line 7 — slots stay stable. Forward reference
  to a not-yet-typed name is a clean name error.
- **Captured `for`/`catch`/`with`/starred bindings**: captured for-var → `[3000, 3000, 3000]` (the
  documented one-scope-per-function late binding, not a bug); captured catch name; captured with name;
  `var first, *rest`; `var *init, last`; catch-name **reuse** across two handlers both captured;
  captured params + captured locals + for-var mixed. All correct, identical under `--gc-threshold 1`.
- **`break`/`continue`/`return` crossing cleanups** (the `$ret`/`$exc` parking): break inside a
  finally; continue inside a try/finally; return crossing a finally that itself contains a
  loop+break; nested finallys; break inside a finally targeting an outer loop; `break` inside a
  `switch` inside a `finally`. All correct, identical under `--gc-threshold 1`.
- **`SaveExcSpan`/`RestoreExcSpan`**: an outer `throw` at line 4 is still blamed at line 4 after a
  finally body handles its own exception at line 7; still correct with 3 levels of nested
  try-in-finally (blames line 5, the real in-flight exception).
- **Switch**: exact type+value (`case 1` ≠ `case 1.0` ≠ `case "1"` ≠ `case True`); `0.0`/`-0.0` share
  an arm (agreeing with `==`); NaN and `inf` fall to `default`; duplicate detection catches
  `case 0.0`/`case -0.0` and `case -1` twice (negated-literal folding works). The `SwitchMatch` chain
  fallback (forced with `var`-valued cases) agrees with the O(1) table on every one of those.
  A 600-case table with keys 1000..1599 (outside the small-int intern range) is correct, and identical
  under `--gc-threshold 1`.
- **Constant dedup**: two `0.0` literals share a const slot (`id(a)==id(d)` True); `0.0` and `-0.0` are
  distinct (`copysign` → `1.0` / `-1.0`); two `100000` literals (outside the intern range) dedup
  correctly. Note `-0.0` parses as `UnaryExpr(Neg, Literal 0.0)`, so it never reaches `addConst` as a
  literal at all — the bit-keying in `scalarConstKey` is correct but defensive (no source form produces
  a NaN or `-0.0` *literal* constant).
- **Deep nesting throws cleanly, never crashes**: 5000 nested parens, 20000-term `+` chain, 3000 nested
  list literals, 5000 unary minuses, 3000 nested `if` blocks, 2000 nested function literals — all
  `error: expression nested too deeply`, exit 1, no crash. The **parser's** nesting bound (~2000) fires
  first, so `Resolver::checkExpr`'s `depth_ > 2800` silent-return and `Compiler::kMaxDepth = 3000` are
  unreachable defense-in-depth via source. 1500 nested `try/finally` (1500 `excSpanSlots`) compiles and
  runs correctly.
- **Compile-time program errors** all throw like parse diagnostics with line/col: positional-after-
  keyword, invalid assignment target (literal and call), two starred targets (both the parser and the
  `AssignStmt` compiler path), unpack count mismatch, duplicate switch `default`, empty switch body,
  `break` outside a loop, `return` outside a function — **including inside a class body** (so `$ret`
  can never be `StoreName`d at class scope, which bounds A02-1 to `$with`/`$exc`).
- **Analyzer rules** all fire correctly and none misfired on the control cases: assigned-but-never-used
  (with `_`-prefix, `self`, and parameters correctly exempted); unreachable code (one warning per
  block); re-declared `var`; self-assignment; unused result for name/index/binary/f-string/member —
  and correctly **not** for a `None` literal or a `discard`ed expression; `todo "msg"`. Warning order
  for 7 unused vars in one scope came out in **source order** and was stable across 3 runs (`fum`
  maps are insertion-ordered flat maps, so `popScope`'s iteration is deterministic — not the
  hash-order flakiness I suspected).
- **serde round-trip** of the `locals.hpp` free-variable analysis: closure over a local, self-recursion
  (`fact`), mutual recursion (`isodd`/`iseven`), a default referencing an earlier param, a captured
  module (reconnects by re-import), a captured builtin. All correct, incl. `--gc-threshold 1`.
- **`B.count` (reading a class var off the class value) throws `has no attribute`** — I flagged this
  then found it is **PINNED by design**: `tools/tests/scripts/verify_operators.ki:387-388`
  ("PIN: Class.count read raises"; "The docs describe reads *through the class* from an INSTANCE
  only"). Not a bug. Residual nit for whoever owns `inspect`: `inspect(B)` still lists `count: Integer`
  among B's members even though `B.count` is unreadable — cosmetic, outside A02's scope.
- **Analyzer false positives already in the `.audit/README.md` table** reconfirmed and NOT re-reported:
  use-before-`var` emits a spurious "assigned but never used" (v1.13 A03-1) — observed in my
  read-before-write probes, left alone per the table.
- **The compiler's `TupleExpr`/`StarExpr` star-rejection throws** (compiler.hpp:719-728) appear
  **unreachable** — the parser rejects `*` in expression position first (`[1, *[2,3]]`,
  `io.print(*xs)`, `var t = 1, *xs` all give `error: expected an expression`). Defensive, not a bug.
- **`Function(a)` with a body `var a`** (a param name reused as a local, *not* a duplicate param) is
  correct — `paramSet` filtering keeps it at the param's env index. Distinguishing this from A02-3 was
  the key to isolating the root cause.

Status: DONE
