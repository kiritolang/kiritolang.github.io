# A02 — Parser + AST Audit (src/kirito/parser.hpp, src/kirito/ast.hpp)

Auditor: A02. Area: Parser + AST. Status: COMPLETE.

Method: full static read of parser.hpp (1072 lines) + ast.hpp (434 lines), cross-referenced
against src/kirito/{compiler,resolver,analyzer,locals,lexer}.hpp and the test suite
(tools/tests/{errors,scripts,unit}). Read-only; no build/run (orchestrator re-verifies).

## Overall assessment
The parser is robust and well-tested. Stack-safety is comprehensive: every recursive-descent
re-entry point is guarded (DepthGuard on parseExpr / parseConditional-else / parseNot / parseUnary /
parsePow-exponent / parseIndentedSuite; ChainDepth on the left-deep operator/postfix loops), and the
main paths (parens, `1+1+...`, `1**1**...`, `a[0][0]...`, `a.b.b...`, indentation pyramids, nested
f-strings) have explicit regression tests (test_audit_regressions.cpp, test_audit_hardening.cpp). No
out-of-bounds token reads (EOF is a hard stop everywhere; advance() is always type-guarded).
Invalid-target / empty-subscript / positional-after-keyword / duplicate-case / two-star / etc. are all
cleanly rejected (parser or compiler) and mostly tested (surf_parse_*, surf_sw_*).

No Critical/High correctness bugs found. Findings are: one subtle silent misparse (Medium), several
diagnostic-quality issues (Low), and a set of coverage gaps + DRY notes.

---

## Findings

### A02-1: Inline function body's trailing comma is silently absorbed by the enclosing packing context
- severity: Medium
- location: src/kirito/parser.hpp:734-778 (parseInlineStatement) vs 307-320 (parseValueSeq); packing
  callers at 140, 266, 285
- category: bug
- description / failure scenario: `var f = Function(): return a, b` does NOT return the pair `a, b`.
  parseInlineStatement's return arm reads its value with `parseExpr()` and explicitly stops at Comma
  (line 739-741), so the inline body becomes `return a` and the FunctionExpr is returned to the
  caller with the `, b` still unconsumed. The caller here is `parseValueSeq` (var init / assign RHS /
  return value all use it), which then treats the comma as a top-level packing separator and builds a
  TupleExpr `[<function>, b]`. So `f` is silently bound to a 2-element List `[function, b]` and the
  function returns only `a` — no error, wrong semantics. Contrast: a block body `return a, b` packs
  correctly (parseReturn -> parseValueSeq), and `discard Function(): return a, b` (parseExpr, not
  parseValueSeq) instead throws "expected end of statement". So the behavior is inconsistent across
  otherwise-equivalent positions and, in the packing positions, silent.
- proposed test: assert `var f = Function(): return a, b` either errors clearly OR (if intended)
  document that inline bodies cannot pack; add a golden test pinning the chosen behavior. Also test
  `var xs = Function(): pass, 1` to expose the same absorption.
- proposed fix: after an inline-bodied Function literal, set `blockJustClosed_ = true` (or an
  equivalent "value is a suite" flag) so the enclosing parseValueSeq/statement comma loop does not
  swallow a following comma; OR make parseInlineStatement's return use parseValueSeq and terminate the
  Function literal at end-of-line. Minimally, reject a comma immediately following an inline Function
  value with a clear diagnostic.
- confidence: medium (traced by hand; orchestrator should run the three snippets to confirm the
  silent-pack vs error split).

### A02-2: f-string embedded-expression error spans are coarse (wrong line/col for multi-field / multiline f-strings)
- severity: Low
- location: src/kirito/parser.hpp:884-908 (parseEmbedded), called at 970 with `t.span`
- category: bug (diagnostics)
- description / failure scenario: every embedded `{...}` in an f-string is parsed via
  `parseEmbedded(inner, t.span)` — always the f-string TOKEN's start span. The sub-Lexer is seeded at
  `span.line, span.col`, and parse errors are re-thrown anchored to `span` (line 901). So for a
  multi-field f-string `f"{a} {bad +}"` the error in the second field reports the first field's
  column, and for a multiline triple-quoted f-string with a field on a later line the error reports
  the f-string's opening line/col, not the field's real location. The comment at 891-894 claims spans
  are "absolute to the source file", but they are only anchored to the token, not offset to the field.
- proposed test: an errors/*.ki with a triple-quoted f-string whose 3rd-line field has a syntax error;
  assert the reported line is the field's, not the opening quote's (documents current behavior at
  minimum).
- proposed fix: compute each field's true (line, col) by advancing the token's start position over the
  literal prefix consumed before the `{`, and seed the sub-Lexer with that.
- confidence: high (spans are demonstrably token-anchored).

### A02-3: coverage gap — `single '}' in f-string` rejection is untested
- severity: coverage-gap
- location: src/kirito/parser.hpp:973-975
- category: coverage-gap
- description: `f"a } b"` (a lone unescaped `}`) throws "single '}' in f-string". The `{{`/`}}`
  literal-escape path IS tested (test_fstring.cpp:18/48, spec_fstrings.ki:51-52) and the unmatched-`{`
  path is tested (fstring_unmatched_brace), but the lone-`}` error has no test.
- proposed test: errors/fstring_single_brace.ki: `discard f"a } b"` with .experr `single '}' in f-string`.
- confidence: high.

### A02-4: coverage gap — StarExpr reaching a value position is untested
- severity: coverage-gap
- location: parser emits StarExpr from parseTargetElement (parser.hpp:158-166); compiler rejects in
  value context at compiler.hpp:644-645, 650-652
- category: coverage-gap
- description: a `*x` that is NOT an unpack target reaches the compiler as a value and throws "starred
  expression is only valid as an assignment target". Reachable via a bare statement `*a`, a value
  tuple `var x = *a, b`, or a starred element inside a list/other value. No test exercises this clean
  rejection (grep for "only valid as" and "starred expression" in tools/tests returns nothing).
- proposed test: errors cases for `*a` (bare stmt), `var x = *a, b`, and `discard [*a]` asserting the
  "starred expression is only valid as an assignment target" message.
- confidence: high.

### A02-5: coverage gap — lone starred assignment target `*a = xs` is untested
- severity: coverage-gap
- location: compiler.hpp:211-234 (a top-level StarExpr target falls through to "invalid assignment
  target")
- category: coverage-gap
- description: `*a = [1, 2, 3]` (a single starred target, not inside a tuple) is accepted by the parser
  (parseTargetElement -> StarExpr, then Assign) and rejected by the compiler as "invalid assignment
  target". The two-star-in-a-tuple case is tested (unpack_two_stars, test_unpacking.cpp:97) but the
  lone-star-target case is not.
- proposed test: errors/lone_star_target.ki: `var xs = [1]\n*a = xs` -> "invalid assignment target".
- confidence: high.

### A02-6: coverage gap — deep `not`/unary-`-`/ternary chains don't exercise their DepthGuards
- severity: coverage-gap
- location: parser.hpp:472-483 (parseNot), 543-554 (parseUnary), 431-448 (parseConditional else)
- category: coverage-gap
- description: test_audit_regressions.cpp deep-nest suite covers index/add/pow/member chains and the
  indentation pyramid, but NOT a 5000-deep `not not ... x`, `--- ... 1`, or `a if c else a if c else
  ...` chain. Each of those has its own DepthGuard (right-recursive, so relies on the guard, not
  ChainDepth) and should be pinned so a regression that drops the guard is caught. (Mechanism looks
  correct; this is purely a missing regression.)
- proposed test: extend the deep-nest suite with `not`*5000, unary-`-`*5000, and a 5000-link ternary
  chain, each asserting "nested too deeply".
- confidence: high.

### A02-7: coverage gap / limitation — inline function body admits only one simple statement; other keywords give a misleading error
- severity: coverage-gap
- location: src/kirito/parser.hpp:734-778 (parseInlineStatement)
- category: coverage-gap
- description: parseInlineStatement handles only return/pass/todo/throw/expr/assign. An inline
  `Function(): var x = 1`, `Function(): if c: ...`, `Function(): assert x`, `Function(): break`,
  `Function(): discard e`, `Function(): while ...`, `Function(): for ...` all fall to the default arm
  and fail with "expected an expression" (the keyword isn't a primary) — a confusing diagnostic for
  what is really "inline bodies allow a single simple statement; use a block body". No test documents
  this surface.
- proposed test: assert the (current) rejection for a few inline non-simple statements, and/or improve
  the message. Also note that inline `break`/`continue` bypass the loop-context check entirely.
- confidence: high.

### A02-8: coverage gap / limitation — multi-axis slice syntax `t[:, i]` / `t[a:b, c]` gives a confusing error
- severity: coverage-gap
- location: src/kirito/parser.hpp:585-626 (subscript parsing)
- category: coverage-gap
- description: the subscript grammar supports either a single slice `[a:b:c]` OR a comma-separated
  index list `[a, b, c]`, but not a mix. `t[:, i]` parses the slice, then on the comma tries the
  `stop`/`step` path and eventually `expect(RBracket)` on the comma -> "expected ']'"; `t[a:b, c]`
  same. Numpy-style multi-axis slicing (advertised loosely in the tensor docs) is thus a grammar
  limitation with an unhelpful message and no test covering the rejection.
- proposed test: errors case pinning the message for `t[:, 0]`; and/or document the limitation.
- confidence: high.

### A02-9: coverage gap — nested f-string format field `f"{x:{w}}"` is taken literally (unsupported), untested
- severity: coverage-gap
- location: src/kirito/parser.hpp:954-969 (spec split)
- category: coverage-gap
- description: the spec-splitter is correctly bracket/quote-aware, so for `f"{x:{w}}"` it extracts
  expr=`x`, spec=`{w}` and passes `{w}` verbatim as the format spec (no nested-field substitution).
  Python supports nested fields here; Kirito does not, and there's no test asserting either the
  behavior or the resulting format error. Worth a doc/test to pin "no nested format fields".
- proposed test: assert `f"{1:{2}}"` errors (bad format spec) or document.
- confidence: high (parser correctly extracts; runtime treats `{w}` as a literal spec).

### A02-10: coverage gap — bare statement-level tuple packing (`a, b` / trailing `a,`) has thin coverage
- severity: coverage-gap
- location: src/kirito/parser.hpp:125-137 (statement-level comma loop)
- category: coverage-gap
- description: a bare `a, b` statement (no `=`) packs to a List value via TupleExpr, and trailing-comma
  forms (`a,`, `a, b,`) are handled by the break checks. `return a, b` packing is tested but the
  bare-expression-statement packing and trailing-comma edges (and the interaction with the tuple loop's
  Assign/Newline/EOF/Dedent break at line 132-133) don't have focused tests.
- proposed test: assert `var t = 1, 2, 3` -> `[1, 2, 3]`, `var s = 1,` -> `[1]`, and a bare `1, 2`
  statement's value/warning behavior.
- confidence: medium (behavior inferred from code; likely partially covered incidentally).

### A02-11: DRY — parseInlineStatement duplicates parseStatement's per-keyword logic with divergent terminators
- severity: dry
- location: src/kirito/parser.hpp:65-155 (parseStatement) vs 734-778 (parseInlineStatement)
- category: dry
- description: return/pass/todo/throw/expr/assign are parsed in two places with subtly different
  terminator handling. The divergence (parseExpr-with-comma-stop vs parseValueSeq) is the root of
  A02-1's silent misparse and A02-7's limited inline surface. Consolidating (a shared "parse one simple
  statement" helper parameterized by terminator policy) would remove the inconsistency risk.
- proposed fix: factor a common simple-statement parser; inline mode only changes the terminator rule.
- confidence: high.

### A02-12: DRY — comma-tuple packing logic duplicated across parseStatement / parseValueSeq
- severity: dry
- location: src/kirito/parser.hpp:125-137, 307-320
- category: dry
- description: two near-identical loops build a TupleExpr with trailing-comma handling; they differ
  only in element parser (parseTargetElement vs parseExpr) and the extra Assign break. A shared helper
  taking those two knobs would DRY them.
- confidence: high.

### A02-13: Low — member/index/call node spans point at the operator token, not the operand
- severity: Low
- location: parser.hpp:628 (MemberExpr span = the `.`), 586 (IndexExpr/SliceExpr span = `[`), 575
  (CallExpr span = `(`)
- category: weak-spot (diagnostics)
- description: runtime errors surfaced against these nodes (e.g. "no such attribute", index errors)
  report the operator/bracket column rather than the receiver or the offending name/key, slightly
  reducing error-location precision. Design choice, not a correctness bug; noting for completeness.
- proposed fix: (optional) set MemberExpr.span to cover object..name; low priority.
- confidence: high.

---

## Notes verified NOT to be bugs (so they aren't re-flagged)
- Invalid assignment target (`1 = 2`, `f() = 2`) — deferred to compiler.hpp:234, TESTED
  (surf_parse_07/08, invalid_assign_target, test_bytecode.cpp:122). Not a parser rejection by design.
- Positional-after-keyword — compiler-time, TESTED (surf_parse_13).
- Duplicate parameter names — analyzer WARNING (not an error), TESTED (test_warnings, r7_language).
- Duplicate case values — compiler-time, TESTED (surf_sw_01/06); second `default` IS parser-time
  (parser.hpp:188), TESTED (surf_sw_02).
- Case-value non-scalar — compiler-time, TESTED (surf_sw_04).
- Empty switch / no case-or-default — parser-time (parser.hpp:198), and empty case suite is caught by
  the Indent requirement (surf_sw_05/07 cover the indented-body messages).
- Empty subscript `obj[]` — parser-time (parser.hpp:619), TESTED (empty_subscript).
- `**` right-assoc / unary interaction (`-2**2 == -4`, `2**3**2 == 512`, `2**-1`) — correct.
- Chained comparison is LEFT-assoc (`1 < 2 < 3` == `(1<2)<3`), i.e. NOT Python chaining — documented
  and TESTED (chained_comparison -> "does not support this binary operator").
- `not`/`and`/`or` precedence relative to comparison — correct.
- Int literal overflow wraps (never throws); float literal overflow -> HUGE_VAL (never throws) — safe.
- No token OOB read: EOF is a hard stop; peekAt clamps; advance is type-guarded.
- Lexer suppresses NEWLINE/INDENT/DEDENT inside ()/[]/{} (parenDepth_), so block-bodied Function
  literals cannot appear inside brackets; blockJustClosed_ therefore cannot leak out of a bracketed
  context (the leak I initially suspected is not reachable).

## Summary counts
- bug: 2 (A02-1 Medium, A02-2 Low) + A02-13 Low weak-spot(diagnostics)
- coverage-gap: 8 (A02-3, A02-4, A02-5, A02-6, A02-7, A02-8, A02-9, A02-10)
- dry: 2 (A02-11, A02-12)
- By severity: Medium 1, Low 2, coverage-gap 8, dry 2.
