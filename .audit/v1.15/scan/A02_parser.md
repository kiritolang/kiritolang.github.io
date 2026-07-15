# A02 Parser

Status: DONE

Scope: src/kirito/parser.hpp, src/kirito/ast.hpp — expression/statement parsing, precedence,
conditional expr, packing/unpacking, kwargs+defaults, type annotations, indentation blocks,
switch/case, try/catch/finally/throw, with, inline vs block Function bodies, source-span capture,
parse-depth guard.

## Findings

### A02-1: `FunctionExpr::source` / `ClassStmt::source` over-capture trailing comments and blank lines  [severity: MED] [confidence: confirmed]
- **Location**: `src/kirito/parser.hpp:1104-1117` (`captureSource`), used from `parseFunction` (line 767)
  and `parseClass` (line 246).
- **What**: `captureSource(startSpan, endSpan)` slices the raw file text from the byte offset of the
  construct's first token to the byte offset of `peek()` — the next REAL token after the body. Since
  `#`-comments and blank lines produce no tokens, any comment lines / blank lines that sit between the
  end of a `Function`/`class` body and the following statement are swept into the "verbatim source"
  text. The trailing `while (!text.empty() && (... whitespace ...)) text.pop_back();` rtrim only strips
  whitespace characters, not comment text, so a trailing `#`-comment line survives verbatim inside the
  captured source. This affects both top-level and nested `Function`s, and `class` bodies.
- **Repro** (reproduced live against `build-release/ki`):
  ```
  var dump = import("dump")
  var io = import("io")
  var f = Function(x):
      return x + 1
  # trailing comment 1
  # trailing comment 2
  io.print(dump.dumps(f))
  ```
  `dump.dumps(f)`'s embedded source blob contains
  `"Function(x):\n    return x + 1\n# trailing comment 1\n# trailing comment 2"` — both comment lines
  leaked into what is documented as "the exact source text of this `Function(...)...` literal" (ast.hpp
  comment on `FunctionExpr::source`). Same reproduction for `ClassStmt::source` with a `class` followed
  by a comment line. The nested-function case (an inner `Function` followed by a same-scope comment
  before the enclosing block's next statement) reproduces identically.
  Functionally the round-trip (`dump.loads` then calling the function / instantiating the class) still
  works, because the comment is harmlessly re-stripped by the re-parse's lexer — so this is not a
  silent-corruption bug. But it is a genuine defect in a **documented, load-bearing serialization
  contract** ("the exact source text... captured verbatim"): it (a) needlessly bloats every serialized
  Function/class value that has a trailing comment or blank lines before the next statement, and (b) can
  leak comment text — which a developer may reasonably not expect to travel over `dump`/`serialize`/
  `net` — into a value that gets persisted or transmitted.
- **Proposed fix**: anchor the end of the captured span to the last token actually **consumed** while
  parsing the body (e.g. the closing `Dedent`, or the last content token before it), not to `peek()`
  (the start of the next, possibly far-away, real token). Track the byte offset just past the
  previously-consumed token (its span's byte offset + its text length, falling back to +1 for
  zero-width struct tokens like `Newline`/`Dedent`) as the running "last real content end", and pass
  that to `captureSource` instead of `peek().span`. This naturally excludes any interleaving comments/
  blank lines since they were never tokenized in the first place.
- **Proposed test**: a C++ unit test (`test_serde.cpp`) or `.ki` golden test that defines a `Function`/
  `class` immediately followed by one or more `#`-comment lines, calls `dump.dumps`, and asserts the
  serialized blob's embedded source text does NOT contain the comment's marker text (e.g. assert
  `"trailing comment" not in blob` after decoding, or assert the captured source's exact expected
  string once the fix lands).

### A02-2: Call-argument lists and `Function` parameter lists reject a trailing comma; List/Set/Dict literals accept one  [severity: LOW] [confidence: confirmed]
- **Location**: `src/kirito/parser.hpp` — `parsePostfix`'s call-arg loop (lines 615-618:
  `if (!at(RParen)) { call->args.push_back(parseArg()); while (at(Comma)) { advance();
  call->args.push_back(parseArg()); } }`) and `parseFunction`'s param loop (lines 734-745, same
  shape) vs. `parseListLiteral` (lines 876-880: `if (!at(Comma)) break;` after each element, tolerating
  a trailing comma before `]`) and `parseBraceLiteral`'s Set/Dict loops (lines 900-906, 913-917: `if
  (at(RBrace)) break;` right after consuming the comma).
- **What**: `[1, 2, 3,]` and `{1, 2,}`/`{"k": 1,}` parse fine (trailing comma tolerated), but
  `f(1, 2,)` and `Function(a, b,):` both throw `"expected an expression"` / `"expected a parameter
  name"` — the call/param loops unconditionally call `parseArg()`/`parseParam()` again after consuming
  a comma, with no "was that the trailing comma before the closer?" check. This is an internal grammar
  inconsistency (not called out in CLAUDE.md, and not present in the README's false-positive table),
  and diverges from the common language convention (Python, Rust, JS all allow a trailing comma in both
  call args and parameter lists once container literals allow it) that this project otherwise mirrors.
- **Repro**:
  ```
  var f = Function(a, b):
      return a + b
  f(1, 2,)          # -> parse error: expected an expression
  ```
  ```
  var f = Function(a, b,):     # -> parse error: expected a parameter name
      return a + b
  ```
- **Proposed fix**: mirror the List/Set/Dict pattern in both loops — after `advance()`ing the comma,
  `if (at(TokenType::RParen)) break;` (call args) and the equivalent for the param loop, before calling
  `parseArg()`/`parseParam()` again.
- **Proposed test**: a small parser/functions unit test asserting `f(1, 2,)` and
  `Function(a, b,): return a+b` both parse (once fixed), alongside the existing List/Set/Dict
  trailing-comma coverage.

## Coverage gaps (untested grammar productions / error paths)

- No test exercises a trailing comma in a call-argument list or a `Function` parameter list (see
  A02-2) — the existing suite only covers trailing commas for List/Set/Dict literals and CSV parsing.
- No test asserts on the exact captured text of `FunctionExpr::source`/`ClassStmt::source` (e.g. by
  round-tripping through `dump`/`serialize` and checking the decoded source string) — the existing
  `test_serde.cpp` / `r*_serde.ki` / `verify_serde.ki` suite checks that serialization *round-trips
  correctly* (values still work after reload) but never inspects the source text itself, which is how
  A02-1 went unnoticed.
- No test covers unicode content preceding a `Function`/`class` literal (same-line or prior lines)
  interacting with `captureSource`'s byte-offset math — verified correct above, but not pinned by a
  regression test.
- No test for a starred target nested inside parens (`(*a), b = xs`) — confirmed it's rejected
  ("expected an expression"); not clear if this is an intentional limitation or worth a clearer
  diagnostic (currently the generic "expected an expression" doesn't hint that a bare `*` isn't a valid
  parenthesized sub-expression).
- No test for a parenthesized nested-tuple assignment target (`(a, b), c = [[1,2], 3]`), which is
  rejected as a parse error ("expected ')'") — Python supports this; Kirito's spec doesn't claim it, so
  this is a documented-limitation gap rather than a bug, but it's untested either way.
- `parseTargetNameList`'s "two starred targets" check (`var`/`for`) has no direct unit test (only
  exercised informally); the general statement-level tuple-target case relies entirely on
  `compiler.hpp:250` and isn't covered from the parser's own test files.
- `atWord("case")`/`atWord("default")` as ordinary identifiers used as parameter names / variable
  names outside a `switch` has no regression test pinning the soft-keyword behavior (manually verified
  above).
- Deep-nesting adversarial cases (parens/lists/calls/unary/member-chains/if-pyramids) are only tested
  for the conditional-expression chain in `test_conditional.cpp`; the general `DepthGuard`/`ChainDepth`
  mechanism as applied to other productions (calls, member chains, indentation pyramids) has no
  dedicated regression test, even though it was manually verified to behave correctly here.

## DRY notes

- `parseInlineStatement` (lines 773-817) duplicates a meaningful fraction of `parseStatement`'s tail
  (`ReturnStmt`/`PassStmt`/`TodoStmt`/`ThrowStmt`/plain-expr-or-`AssignStmt` construction) with only the
  statement-terminator handling differing (no `expectStatementEnd`/`endSimpleStatement` call, and the
  inline `KwReturn` case additionally guards against absorbing a following `,`/`)`/`]`). This is a
  deliberate divergence (inline bodies don't call `expectStatementEnd`), so a full merge isn't free, but
  the two `ReturnStmt`/`ThrowStmt`/`PassStmt`/`TodoStmt` node-construction blocks are otherwise
  byte-for-byte identical and could share a helper that takes the "does this consume a trailing
  newline" behavior as a parameter. [severity: LOW] [confidence: likely] — cosmetic, not a bug.

## Verified correct-by-design (checked against README false-positive table + spec; not re-flagging)

- `ChainDepth`/`DepthGuard` parse-depth guard: adversarially tested with 3000-deep parens, 5000-deep
  list/bracket literals, 100000-term `+` chains, 5000-deep call chains, 200000-deep unary `-` chains,
  5000-deep member-access chains, and 2000-deep `if:` indentation pyramids. Every one throws a clean
  `expression nested too deeply` at parse time; none crash or overflow the native stack (verified live
  against `build-release/ki`).
- Positional-after-keyword (`f(a=1, 2)`) and two-starred-target (`a, *b, *c = xs`, `for *a, *b in xs`)
  are correctly rejected — but the checks live in `compiler.hpp` (positional-after-keyword,
  runtime-catchable "positional argument follows keyword argument") and both `parser.hpp` (the `var`/
  `for` name-list path, `parseTargetNameList`) and `compiler.hpp:250` (the general statement-level
  tuple-target path via `walkAssignTarget`) for two-starred-targets. Confirmed no parser gap.
- Inline-function comma-pack rejection (`Function(): return a, b` at top level, and mid-pack) throws
  the documented clear diagnostic; a comma inside a call argument list after an inline body (which
  delimits normally, not packs) works correctly and is unaffected.
- Chained conditional right-associativity (`a if c1 else b if c2 else c`) and full precedence
  ordering (arithmetic > comparison > not > and > or > conditional) verified via the existing
  `test_conditional.cpp` fuzz harness (4000+2000 randomized cases) plus manual adversarial checks —
  all correct.
- `case`/`default` soft keywords: confirmed usable as ordinary identifiers (`var default = 5`,
  `var case = 10`) outside a switch body.
- f-string embedded-expression parse errors are deliberately re-anchored to the f-string token's own
  span (not the exact failing sub-position) — by design, per the code comment; confirmed this doesn't
  regress to line-1-relative spans (sub-parser is seeded with the outer span).
- Lexer column tracking is byte-based, not code-point-based (`lexer.hpp:120`, `advance()` increments
  `col_` once per byte unless it's `\n`), consistently for the whole file. `Parser::byteOf`/
  `captureSource` therefore compute correct byte offsets even when unicode (multi-byte UTF-8) text
  precedes a `Function`/`class` literal on an earlier line or the same line — confirmed by a live
  round-trip test (`dump.dumps`/`dump.loads` of a function following a `"日本語"` string literal
  round-trips correctly). No byte/codepoint mismatch found.


## Summary

Scanned `src/kirito/parser.hpp` (1129 lines) + `src/kirito/ast.hpp` (459 lines) in full, plus the
relevant slices of `compiler.hpp`/`resolver.hpp`/`locals.hpp`/`lexer.hpp` needed to confirm whether a
suspected parser gap is actually handled downstream. Adversarially tested against a live
`build-release/ki` binary: parse-depth guard (deep parens/lists/calls/unary/member-chains/if-pyramids
— all clean errors, no crash), two-starred-target rejection, positional-after-keyword rejection,
inline-function comma-pack rejection, chained-conditional right-associativity, soft-keyword `case`/
`default` reuse as identifiers, invalid-assignment-target rejection (literal/call-result/conditional
targets), malformed `try`/`switch`/`with`/`class` bodies, and unicode-before-Function byte/codepoint
column handling — all found correct and matching CLAUDE.md's documented behavior or the audit
README's false-positive table.

Found 2 real findings and 1 DRY note:
- **A02-1 (MED, confirmed)**: `FunctionExpr::source`/`ClassStmt::source` capture trailing `#`-comments
  and blank lines past the construct's actual end (because `captureSource` anchors on the next real
  token, and comments produce no tokens) — bloats and can leak comment text into every
  `dump`/`serialize`d Function or class value that's followed by a comment. Round-trip still works
  (comments are harmlessly re-stripped on reload), so this is a data-hygiene bug, not a correctness
  crash.
- **A02-2 (LOW, confirmed)**: call-argument lists and `Function` parameter lists reject a trailing
  comma (`f(1, 2,)`, `Function(a, b,):`) while List/Set/Dict literals accept one — an internal grammar
  inconsistency, untested either way.
- DRY note (LOW): `parseInlineStatement` duplicates several statement-construction blocks already in
  `parseStatement`'s tail; a shared helper could remove the duplication.

Enumerated 8 coverage gaps (see above), the most actionable being: no regression test pins the exact
captured `source` text of a Function/class (which is how A02-1 went unnoticed), and no test covers
trailing commas in call-args/param-lists (which is how A02-2 went unnoticed).

Key files: `/home/user/kiritolang.github.io/src/kirito/parser.hpp`,
`/home/user/kiritolang.github.io/src/kirito/ast.hpp`.
