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

### A01-N4: UTF-8 BOM at file start is a hard lex error
- severity: low
- location: src/kirito/lexer.hpp:53-65 (normalizeNewlines) / main loop 71-100 (no BOM skip)
- category: correctness (portability)
- description: A leading UTF-8 BOM (EF BB BF) is not stripped. The first BOM byte (0xEF) is not
  whitespace/digit/quote/prefix and (in the C locale) not isalpha, so it reaches `op()` and throws
  `unexpected character`. Editors on Windows (Notepad, some VS Code configs) and various tools emit a
  BOM by default, so a perfectly valid `.ki` file saved that way fails to run with a cryptic message.
  Kirito already normalizes CRLF for exactly this Windows-friendliness reason; a BOM skip is the same
  class of fix.
- failure-scenario: CONFIRMED. A file beginning with the 3 BOM bytes then `var x = 1` ->
  `1:1: error: unexpected character '<ef>'`.
- proposed-test: a unit/`.ki` test feeding a BOM-prefixed source and asserting it runs (or a lexer
  unit test on `normalizeNewlines`/tokenize output).
- proposed-fix: in `normalizeNewlines` (or the ctor), strip a leading `\xEF\xBB\xBF` before lexing.
- confidence: high

### A01-N5: `1.e5` silently parses as member access `(1).e5` (runtime attribute error) rather than a float or a clean lex error
- severity: low
- location: src/kirito/lexer.hpp:203 (`peek()=='.' && isdigit(peek(1))` requires a digit AFTER the dot)
- category: correctness (diagnostics / silent misparse)
- description: The float-fraction rule requires a digit immediately after the dot, so `1.e5` lexes as
  Integer `1`, then `.`, then identifier `e5`, i.e. a member access `(1).e5`. This is accepted by the
  parser and only fails at RUNTIME with `type 'Integer' has no attribute 'e5'`. A user who wrote a
  float literal in the common `1.e5` form (valid in Python/C) gets a confusing runtime attribute error
  rather than a value or a clear "invalid float literal". (The `5.` and `.5` forms are already
  documented-rejected; `1.e5` is the untested middle case that misparses instead of erroring cleanly.)
- failure-scenario: CONFIRMED. `var x = 1.e5` -> `line 1, in <module> ... type 'Integer' has no
  attribute 'e5'` at runtime.
- proposed-test: errors/*.ki pinning the intended behaviour of `1.e5` (either accept as 1e5, or a
  clean lex-time "invalid numeric literal").
- proposed-fix: flag only — decide the contract. If `1.` stays rejected, consider detecting `<int>.`
  followed by `e`/`E` and emitting a lex error, or accept `1.e5`/`1.` as floats.
- confidence: high

### A01-N6: error messages embed raw control bytes (NUL, etc.) verbatim
- severity: low
- location: src/kirito/lexer.hpp:408 (`"unexpected character '" + c + "'"`), and any diagnostic that
  interpolates a source byte
- category: correctness (diagnostics)
- description: `op()`'s default arm builds the message by concatenating the offending byte directly.
  For a NUL byte the message contains an embedded `\0` (truncating the visible string on most
  terminals / in C-string handling); for other control bytes or high bytes it prints raw/garbled
  (`unexpected character '<ef>'`, `'^L'`, `'M-C'`). The diagnostic is technically emitted but
  unreadable / self-truncating.
- failure-scenario: CONFIRMED. A bare NUL byte in source -> message string contains an embedded NUL;
  a form-feed -> raw FF byte in the message; a UTF-8 lead byte -> a raw high byte.
- proposed-test: a lexer unit test asserting the message for a NUL/form-feed byte renders the byte as
  an escaped form (e.g. `\x00`) rather than raw.
- proposed-fix: render non-printable bytes as `\xHH` (hex-escape) when building "unexpected character"
  messages.
- confidence: high

### A01-N1-addendum: trailing-comma asymmetry is broader than first noted
- Confirmed by probe: trailing comma is ACCEPTED in `var` targets (`var a, = x`), `for` targets
  (`for a, in xs`), `return`/value-seq (`return 1,`), list/dict/set literals, and statement-level
  tuple targets. It is REJECTED only in the three loops that lack the `if (at(closer)) break;` guard:
  call args (parser.hpp:597), subscript extra-index list (617-620), Function params (721-723). So the
  three rejecting sites are the outliers, which makes the fix clearly "add the break" (uniform accept)
  rather than "reject everywhere".

### A01-N7: DRY — parseFString hand-rolls the same quote/escape skip-scanner twice
- severity: dry
- location: src/kirito/parser.hpp:952-963 (brace-matching scan) and 974-986 (spec-splitting scan)
- category: dry
- description: Within `parseFString`, two loops implement the identical "advance over source while
  tracking whether we're inside a `'`/`"` string literal, honouring `\`-escape" state machine — once
  to find the matching `}`, once to find the `:format-spec` separator at bracket depth 0. They share
  the `quote`/`esc` (resp. `sq`/`sesc`) idiom verbatim and can drift (e.g. one could be taught about a
  new quote form and the other not). A small `struct StringScanState { char quote=0; bool esc=false;
  bool step(char c); }` (returns whether `c` is "structural", i.e. outside a string) would unify them.
- proposed-fix: extract the quote/escape skip step into one helper used by both scans.
- confidence: high

### A01-N8: DRY (persisting from v1.13) — parseInlineStatement duplicates parseStatement; two comma-tuple loops
- severity: dry
- location: src/kirito/parser.hpp:65-155 (parseStatement) vs 751-795 (parseInlineStatement);
  125-137 vs 307-328 (comma-tuple packing)
- category: dry
- description: v1.13's A02-11 / A02-12 are still present. parseInlineStatement re-implements the
  return/pass/todo/throw/expr/assign parsing with a different terminator policy (the source of the
  N2 RBrace-stop-set inconsistency), and the statement-level tuple loop (125-137) and value-seq tuple
  loop (307-328) are near-duplicates differing only in element parser + break conditions. Flagged
  again because the divergence keeps producing the small inconsistencies above (N1 trailing comma, N2
  bare return). Consolidating is the durable fix.
- proposed-fix: a shared "parse one simple statement" helper parameterized by terminator policy, and a
  shared comma-tuple helper parameterized by element parser + break set.
- confidence: high

## Coverage gaps (C++ + .ki)
- A01-C1 (coverage-gap): No test exercises trailing-comma acceptance/rejection anywhere (N1). Prize:
  the asymmetry (calls/subscripts/params reject; everything else accepts) is completely unpinned, so a
  future "add the break" fix has no regression guard and the current rejection isn't documented either.
- A01-C2 (coverage-gap): No "unexpected indent" test — a leading-indented top-level line (N3) is
  untested; the generic "expected an expression" it currently emits is not pinned.
- A01-C3 (coverage-gap): No BOM test (N4) — nothing feeds a BOM-prefixed source.
- A01-C4 (coverage-gap): `1.e5` / `<int>.<e-exp>` misparse (N5) untested; `5.`/`.5` are covered but
  the `1.e5` middle case is not.
- A01-C5 (coverage-gap): the `single '}' in f-string` error (parser.hpp:992) and the multi-field /
  multiline f-string span coarseness (A02-2, still present — CONFIRMED `f"""\n...\n{1 +}"""` reports
  the opening line/col, not the field's) remain unpinned by an asserted-message test.
- A01-C6 (coverage-gap): control-byte-in-diagnostic (N6) untested — no test asserts a NUL/form-feed
  byte is rendered readably in an "unexpected character" message.
- Note (NOT a gap): A01-1/A01-2 (f-string `\xHH` high byte, unknown escape) ARE now covered in
  tools/tests/unit/test_audit_v113.cpp:276-284 — v1.13's A01-7 gap is closed.

### A01-N9: column tracking is byte-based, not code-point-based (persisting A01-10)
- severity: low
- location: src/kirito/lexer.hpp:114-117 (`advance()` increments col_ per byte)
- category: correctness (diagnostics)
- description: `advance()` bumps `col_` once per byte, so after a multi-byte UTF-8 char the reported
  column is a byte offset, not a character offset. CONFIRMED: with `var s = "éé" @` the stray `@` is
  reported at col 16 (byte) though it is the 14th code point. Minor but means editor "go to col N"
  lands wrong on lines containing non-ASCII. Still unpinned by any test.
- proposed-fix: flag only (advance col by 1 per UTF-8 lead byte, or document byte-column contract).
- confidence: high

## Summary (A01 v1.14)

Subsystem is well-hardened after v1.13 — **no High/Medium correctness bugs found**. Escape decoding
is now DRY (decodeCookedEscape), inline-fn comma-pack is rejected, all recursion-depth resource
guards (parens/lists/dicts/members/pow/not/unary/ternary/f-string nesting/indentation-pyramid) throw
cleanly, and a near-limit if-pyramid tears down without crashing. Fresh findings are all low-severity
papercuts + DRY + coverage:

Counts by severity: high 0 · medium 0 · low 7 (N1,N2,N3,N4,N5,N6,N9) · dry 2 (N7,N8) ·
coverage-gap 6 (C1–C6).

Top findings:
1. **A01-N1 (low, CONFIRMED)** — trailing comma is accepted in var/for/return/list/dict/set targets
   but REJECTED in call args, subscripts, and Function params, with the unhelpful "expected an
   expression" message. The three rejecting loops are the outliers; a one-line `if (at(closer)) break;`
   each unifies the behaviour.
2. **A01-N4 (low, CONFIRMED)** — a UTF-8 BOM at file start is a hard `unexpected character` lex error;
   BOM-saving editors (Windows) produce valid `.ki` files that won't run. Same Windows-friendliness
   class as the existing CRLF normalization; strip it in `normalizeNewlines`.
3. **A01-N5 (low, CONFIRMED)** — `1.e5` silently parses as member access `(1).e5` and fails only at
   runtime ("Integer has no attribute 'e5'") instead of being a float or a clean lex error.
4. **A01-N2 (low, CONFIRMED)** — bare `return` before `}` in an inline function body errors, while
   the same before `)`/`]` is a valid empty return (RBrace missing from the stop set).
5. **A01-N3 (low, CONFIRMED)** — leading indentation gives generic "expected an expression" rather
   than "unexpected indent".

DRY: N7 (two hand-rolled quote/escape skip-scanners inside parseFString) and N8 (v1.13's
parseInlineStatement-vs-parseStatement + dual comma-tuple loops still present — root cause of N1/N2).

All findings reproduced with build-debug/ki. Confidence high on every CONFIRMED item.
