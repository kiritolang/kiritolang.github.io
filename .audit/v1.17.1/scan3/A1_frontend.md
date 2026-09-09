# A1 — FRONTEND deep audit (scan3, v1.17.1)

Scope: lexer.hpp, parser.hpp, ast.hpp, analyzer.hpp, compiler.hpp, locals.hpp, resolver.hpp,
common.hpp (frontend parts). All probes run on `build-bin/ki-asan` (HEAD, ASan/UBSan). Hard rule
applied: a finding counts only if a `.ki` program provably breaks. Full source of every file was read.

## Result

**CONFIRMED user-code-provable frontend bugs: 0** (HIGH 0 / MED 0 / LOW 0).

No parse edge case, span, analyzer, constant-fold, or slot/closure-capture defect could be triggered
from user code. Below is the negative-result evidence plus informational (non-counting) observations.

---

## Findings

None confirmed. No SUSPECTED leads either — every avenue investigated resolved to correct behavior
(see Verified CORRECT).

### Informational (NOT bugs under the hard rule — no user-code breakage)

- **analyzer.hpp:78,116-128 · LOW · lint under-warning (false NEGATIVE), documented-acceptable.**
  `pendingUsed_` is an Analyzer-lifetime member, not reset per function. A free-name reference in one
  function that stays pending (a builtin/global) is consumed by a later `declare()` of the same name in
  an unrelated function, marking that later binding already-used.
  Reproducer:
  ```
  var a = Function():
      discard len          # 'len' -> pendingUsed_
      return 1
  var b = Function():
      var len = 99         # truly unused, but NOT flagged (pendingUsed_ erased it)
      return 1
  ```
  Actual: no "variable 'len' is assigned but never used" warning for `b`. Expected (ideal): warn.
  This is a *missed* lint warning, never a false positive, and the code comment
  (analyzer.hpp:124-128) explicitly documents it as the acceptable tradeoff. Program behavior is
  unaffected. Not counted. Fixing would mean scoping `pendingUsed_` per function-analysis.

- **lexer.hpp:197-248 · underscore digit separators unsupported.** `1_000` lexes as `1` then
  identifier `_000`, yielding a clean `expected end of statement` parse error. Not documented as a
  feature (docs list only decimal/hex/oct/bin/scientific), so this is not a divergence — noted only in
  case a future feature round wants separators. No silent miscompile.

- **parser.hpp:1124-1142 / lexer.hpp:197-248 · leading-zero decimal `0777 == 777`.** Confirmed intended
  and documented (docs/pages/02-language-guide.md:108-109: "a leading zero does not mean octal —
  `007 == 7`"). Correct, not a bug.

---

## Verified CORRECT (probed, sound)

Every item below was executed on ki-asan and produced the correct result with a clean exit (no crash,
no ASan/UBSan trace).

**Numeric literals / int64 boundary (lexer.number, parser.intLiteral)**
- `9223372036854775807` → max; `9223372036854775808` → INT64_MIN (documented two's-complement wrap);
  `-9223372036854775808` → INT64_MIN; `0xFFFFFFFFFFFFFFFF` → -1; `99999999999999999999999` wraps, no
  throw. Matches the "defined integer overflow" contract; `std::stoll`-style abort is avoided.
- Floats: `5e-324` → 4.94e-324 (subnormal accepted, no `std::out_of_range` crash — parseDouble path);
  `1e400` → inf (overflow → +inf, not throw); `1.e5` → 100000.0 (C-family `1.e5` fraction rule).
- `(5).compare(5)` and `1 .compare(2)` both lex/parse correctly — the exponent-lookahead in
  `number()` correctly declines to eat `.compare` as a fraction.

**f-strings (parser.parseFString / parseEmbedded, common.decodeCookedEscape)**
- `f"val={x}"`, `f"{x:05d}"` (spec), `f"{{literal}}"` (escaped brace), `f"{d['a']}"` (quote-aware brace
  scan), `f"{x if x>3 else 0}"` (ternary), `f"a{f'b{x}c'}d"` (nested f-string, alternating quotes),
  `f"\x41\n end"` (cooked escape → 'A'), `rf"raw\n{1+1}"` (raw prefix keeps `\n` literal, still
  interpolates). All correct.
- Quote-aware `:` spec split: `f"{'a:b'}"` → `a:b` (colon inside string not treated as spec sep).
- Leading-space trim: `f"{ 3 }"` → `3`.
- Error paths: `f"{}"` → "must contain a single expression"; `f"{x"` → "unmatched '{' in f-string";
  single `}` → "single '}' in f-string". All clean KiritoErrors, no crash.

**Deep-nesting stack-overflow gates (parser DepthGuard/ChainDepth; compiler/resolver/analyzer bounds)**
- 5000-deep parens, 20000-link `+` chain, 20000-link `[0]` index chain, 400-deep nested f-strings:
  each throws `expression nested too deeply` cleanly (no SIGSEGV, no ASan stack overflow). The parser
  gate (kMaxParseDepth = 250 under sanitizer) is the tightest, so the looser compiler(3000)/
  resolver(2800)/analyzer(2500) bounds are never reached with an over-deep tree — no downstream
  overflow. Left-deep chains are correctly counted via ChainDepth (teardown-safe).

**Closure / slot layout (compiler.assignLocalSlots, locals.capturedLocals, resolver.computeFunctionEnvIndex)**
- Mutable closure counter (`counter` captured + reassigned in nested fn): 1,2,3. Correct.
- Forward capture (nested fn refs a local declared *later* in the outer body): 42. Correct — resolver
  membership + compiler env-slot ordering (collectBlockDeclsOrdered) agree.
- Loop-variable capture: 2,2,2 — shared-scope late binding (documented; loop var lives in enclosing
  scope). Correct, consistent across resolver/compiler/runtime.
- Parameter default referencing an earlier param, with both captured by a nested fn
  (`Function(n, size = n): ...`): `mk(5)`→10, `mk(5,10)`→15. Correct (A03-1/A03-3 paths hold).
- Class methods reading sibling methods and class vars by bare name (`getx` calls `helper` reading
  class var `origin`): 7. Correct — class-body env indexing matches.

**Analyzer warnings (true/positive/negative)**
- Shadowing: `var a` in a nested `if` block when outer already has `var a` → correct "shadows an outer
  'a'" warning; runtime still rebinds (prints 2). Correct.
- Unused: function-local `var unused = 99` → warned; module-level and params/`self`/`_`-prefixed
  correctly exempt.
- Unreachable: statement after `return` → warned once.
- Self-assignment / re-declared / duplicate-param paths all exercised elsewhere in suite.

**Subscript / slice compilation (parser.parsePostfix parseElem, compiler.visit Index/Slice)**
- List: `l[1:3]`, `l[::2]`, `l[:]` correct; slice-assign `l[1:3]=[9,9]` → `[1,9,9,4,5]`.
- List multi-index `l[1,2]` → clean runtime error "type 'List' takes exactly one index" (not a crash).
- Tensor multi-axis: `t[:,1]`, `t[0,:]`, `t[...,0]`, `t[0,1]`, `t[None,:]` (newaxis) all correct.

**Parser rejections (correct, precise spans)**
- Chained comparison `1 < 2 < 3` → rejected with actionable message.
- Duplicate parameter `Function(a, a)` → hard parse error at the second `a`'s span.
- Unterminated string, invalid escape `\q` → clean lex errors with correct line:col.

**switch constant folding + key semantics (compiler.foldConstValue/foldConstKey, scalarSwitchKey)**
- `case True` and `case 1` coexist and dispatch distinctly (subject 1 → int arm); `case 1` vs
  `case 1.0` distinct (subject 1.0 → float arm). No cross-type key collision, no spurious "duplicate
  case". Folded labels reuse the VM's own operators/key function, so labels key identically to a
  runtime subject.

---

## Method notes
- No source was edited, built, or committed; ki-asan reused as-is (built from HEAD).
- Probe scripts written to /tmp (p_*.ki, c*.ki, deep*.ki, fs*.ki, an*.ki, sub/cmp/dup/meth/unt/esc,
  t1/t3, sw2, pn, fe/fu). All runnable to reproduce.
