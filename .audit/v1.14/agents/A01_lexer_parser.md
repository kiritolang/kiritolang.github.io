# A01 — Lexer + Parser + AST Audit (v1.14)

Scope: `src/kirito/{lexer,parser,ast,common}.hpp`. Read-only on src/. Auditor A01.
Date: 2026-07-06.

## Status of prior (v1.13) findings — re-verified
- A01-1/A01-2/A01-3 (f-string `\xHH` raw byte, silent unknown-escape, escape DRY): **FIXED** —
  `decodeCookedEscape` (common.hpp:108) is now the single source of truth, called by both the
  lexer (`decodeEscape`) and `parseFString`. Confirmed by reading.
- A02-1 (inline-fn comma pack silent misparse): **FIXED** — `rejectInlineFnPack` (parser.hpp:331)
  now throws a clear diagnostic.
- A02-2 (f-string embedded-expr spans): **PARTIALLY FIXED** — Lexer now seeds startLine/startCol
  and parseEmbedded re-anchors, but always to `t.span` (the f-string token start), NOT offset per
  field. Multi-field / multiline f-strings still report the opening column. Still a limitation.
- A01-4 (dead `\r` in main-loop skip predicate): **NOT FIXED** — lexer.hpp:78 still has `|| c=='\r'`.
- A01-6 (locale-dependent `std::isalpha`/`isalnum` for identifiers): **NOT FIXED**.
- A01-11 / A02-13 (indentation error span always col 1; node spans at operator token): **NOT FIXED**.

## New findings (v1.14)

### A01-N1: Trailing comma inconsistently allowed — OK in list/dict/set, rejected in call args / index / params
- severity: low
- location: src/kirito/parser.hpp:597 (call-arg loop), 617-620 (index extra-index loop), 721-723 (param loop) vs 856-857 (list), 878-884 (dict), 891-895 (set)
- category: correctness (consistency / diagnostics)
- description: The list/dict/set literal loops break on the closing bracket after a comma, so a
  trailing comma is accepted (`[1,]`, `{1:2,}`, `{1,2,}` all work). The call-argument loop
  (`while at(Comma){advance; args.push(parseArg())}`), the subscript extra-index loop, and the
  Function parameter loop do NOT check for the closer after consuming the comma, so a trailing comma
  falls into `parseArg`/`parseExpr`/`parseParam` on the closing token and produces the confusing
  diagnostic "expected an expression" (calls/index) or "expected a parameter name" (params). Trailing
  commas are a common, harmless editing artifact; the split behaviour is surprising and the message is
  unhelpful (doesn't mention the trailing comma).
- failure-scenario: CONFIRMED. `f(1,)` -> `error: expected an expression`; `a[1,]` -> `expected an
  expression`; `Function(a,): ...` -> `expected a parameter name`. Meanwhile `[1,]`, `{1:2,}`,
  `{1,2,}` all parse fine.
- proposed-test: golden `.ki` asserting `f(1,)`, `a[1,]`, and `Function(a,):` all parse (if the fix
  allows them) OR errors/*.ki pinning a clear "trailing comma" message (if intentionally rejected).
- proposed-fix: mirror the list-loop pattern — after `advance()` past the comma in the three loops,
  `if (at(closer)) break;` so a trailing comma is accepted uniformly. (Or, if the intent is to reject,
  emit a specific "unexpected trailing comma" diagnostic instead of "expected an expression".)
- confidence: high

### A01-N2: Bare `return` before `}` in an inline function body errors, while before `)`/`]` it is a valid empty return
- severity: low
- location: src/kirito/parser.hpp:756-757 (parseInlineStatement return-value stop set)
- category: correctness (consistency)
- description: parseInlineStatement's `return` arm only treats the return as value-less when the next
  token is Newline/EOF/Comma/RParen/RBracket. It omits RBrace. So an inline-bodied function ending a
  bare `return` immediately before a `}` (reachable because a `{...}` set/dict suppresses newlines and
  can hold an inline function) tries `parseExpr()` on the `}` and throws "expected an expression",
  whereas the same bare `return` before `)` or `]` yields a proper value-less ReturnStmt.
- failure-scenario: CONFIRMED. `discard {Function(): return}` -> `error: expected an expression` at
  the `}`, while `discard [Function(): return]` and `discard (Function(): return)` both parse (rc=0).
- proposed-test: errors/*.ki (or a golden) pinning that `{Function(): return}` behaves like the `[]`/
  `()` forms.
- proposed-fix: add `!at(TokenType::RBrace)` to the stop-set condition at parser.hpp:756-757.
- confidence: high

### A01-N3: Leading indentation at top level reports "expected an expression" instead of "unexpected indent"
- severity: low
- location: src/kirito/parser.hpp:845-847 (parsePrimary default) — an Indent token reaches parsePrimary
- category: correctness (diagnostics)
- description: A source file whose first (or any top-level) logical line is indented emits a leading
  Indent token (handleIndentation pushes an indent level relative to the base {0,0}). parseProgram
  only skips Newline, so the Indent flows into parseStatement -> parseTargetElement -> parseExpr ->
  parsePrimary, which has no Indent case and throws the generic "expected an expression". Python emits
  the far clearer "unexpected indent". The CLAUDE.md contract stresses "a message a user can act on".
- failure-scenario: CONFIRMED. A file beginning `   var x = 1` -> `1:4: error: expected an expression`
  (should be an unexpected-indent diagnostic).
- proposed-test: errors/unexpected_indent.ki: a leading-indented line asserting a clear indent error.
- proposed-fix: in parseStatement (or parseProgram) detect a stray Indent/Dedent token and throw
  "unexpected indent" / "unexpected dedent" with the token span.
- confidence: high
