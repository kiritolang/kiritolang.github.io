# A01 Lexer

Status: scan complete. 6 findings (0 HIGH, 2 MED, 4 LOW); several are re-confirmations of
unfixed low-severity items from v1.14's A01 (BOM, `1.e5`, byte-column, leading-indent
diagnostic) — still present in the current code, verified against `build-debug/ki`. One
finding (A01-3) is a new angle connecting the v1.14 "control byte in diagnostic" gap to a
concrete, previously-unnoted failure mode (non-ASCII identifier bytes). No crashes, no OOB
reads, no hangs found — every adversarial input tried produces a clean `KiritoError` with a
line/col. `decodeCookedEscape` (common.hpp) is bounds-checked correctly in every case tried
(`\x` at EOF, `\x` with 0/1 hex digits, dangling backslash). Indentation ambiguity detection
(wide=tab-as-8 vs narrow=tab-as-1) was stress-tested with several multi-level tab/space
mixes and held up — no false negatives or false positives found; the stack-consistency
invariant (each level pairwise-validated against its immediate parent at push time) makes
skipping intermediate-level checks during dedent safe by induction.

Existing coverage is strong: `test_string_literals.cpp` covers all 8 plain forms x quoting x
escapes with two randomized fuzz loops (1500 iters each) against a C++ oracle;
`tools/tests/errors/surf_lex_01..16.ki` pin every "invalid numeric literal", bad `\x`/`\q`
escape, unterminated string (every quote/triple/raw combination), stray-character, and the
one documented tab/space ambiguity case, each with an `.experr`. `test_eval_arith.cpp:69`
already covers `0b2`. `test_lexer.cpp`/`test_indent.cpp` cover CRLF/lone-CR normalization and
the core indent/dedent/ambiguous-tab cases.

---

### A01-1: UTF-8 BOM at file start is a hard lex error  [severity: LOW] [confidence: confirmed]
- **Location**: src/kirito/lexer.hpp:53-65 (`normalizeNewlines`) and the main dispatch loop
  (72-116) — no BOM skip anywhere before tokenization.
- **What**: A leading UTF-8 BOM (`EF BB BF`) is not stripped. The first BOM byte (`0xEF`) is
  not whitespace/digit/quote/prefix, and `isalpha(0xEF)` is false in the "C" locale, so it
  reaches `op()`'s default branch and throws `unexpected character`. Editors/tools that emit a
  BOM by default (Notepad, some Windows toolchains) produce a `.ki` file that is otherwise
  perfectly valid but refuses to run. Kirito already normalizes CRLF for exactly this class of
  Windows-friendliness reason (see the comment on `normalizeNewlines`); a BOM strip is the same
  kind of fix and belongs in the same function.
- **Repro**: a file whose first 3 bytes are `EF BB BF` followed by `var x = 1\n...` fails with
  `1:1: error: unexpected character '<0xEF, shown mangled>'`. Reproduced live:
  ```
  $ printf '\xEF\xBB\xBFvar x = 1\n...' > bom.ki && ki bom.ki
  bom.ki:1:1: error: unexpected character '<EF>'
  ```
- **Proposed fix**: in `normalizeNewlines` (or the `Lexer` ctor before it), if the input starts
  with the 3-byte BOM sequence, skip it before scanning. One `if` at the top of
  `normalizeNewlines`, no other change — the rest of the pipeline is unaffected since the BOM
  never becomes a token.
- **Proposed test**: a unit test constructing `Lexer("\xEF\xBB\xBFvar x = 1")` and asserting
  tokenization succeeds and yields the same token stream as the same source without the BOM;
  and/or a `tools/tests/scripts/*.ki`-style smoke test (harder to author with a literal BOM in
  a text file — a C++ unit test is the natural home).

### A01-2: `1.e5` silently parses as member access `(1).e5` instead of a float or a clean error  [severity: LOW] [confidence: confirmed]
- **Location**: src/kirito/lexer.hpp:208 (`number()`) — the float-fraction rule requires a
  digit immediately after the `.` (`peek() == '.' && isdigit(peek(1))`); when the digit is
  absent (as in `1.e5`, where `e` follows), the `.` is left for `op()` to tokenize as `Dot`,
  and `e5` is lexed as a separate identifier.
- **What**: `1.e5` (a common float spelling in Python/C/JS) becomes three tokens: `Integer(1)`,
  `Dot`, `Identifier(e5)`. This parses successfully as member access `(1).e5` and only fails at
  **runtime** with a confusing `type 'Integer' has no attribute 'e5'`, rather than either
  producing the float `100000.0` or failing with a clear lex-time diagnostic. `5.` and `.5`
  (bare trailing/leading dot) are documented-rejected/untested elsewhere, but `1.e5` is the
  untested middle case that actively misparses rather than erroring cleanly.
- **Repro**: reproduced live —
  ```
  $ echo 'var x = 1.e5
  import("io").print(x)' | ki /dev/stdin
  ...: error: type 'Integer' has no attribute 'e5'
  ```
- **Proposed fix**: in `number()`, after consuming the integer part, also treat `.` followed by
  `e`/`E` (then a valid exponent) as starting a float with an empty fraction — i.e. broaden the
  float-fraction condition to `peek(1) is a digit OR (peek(1) is e/E and a well-formed exponent
  follows)`. Minimal, localized to the existing float-detection block; no change to `5.`/`.5`
  handling.
- **Proposed test**: `var x = 1.e5` (and `2.E-3`) should equal `100000.0` / `0.002`; add
  alongside the existing scientific-notation cases in `test_lexer.cpp` or
  `test_string_literals.cpp`'s numeric-literal peers.

### A01-3: non-ASCII bytes mid-identifier silently truncate the identifier, then surface as an unescaped raw byte in the error message  [severity: MED] [confidence: confirmed]
- **Location**: src/kirito/lexer.hpp:229-235 (`identifier()`, uses
  `std::isalnum(static_cast<unsigned char>(peek()))`) interacting with `op()`'s default branch
  (412-415, `"unexpected character '" + c + "'"`).
- **What**: `identifier()`'s continuation test is `isalnum`/`_` only, which in the default "C"
  locale accepts ASCII only. A UTF-8 identifier like `café` lexes `c`,`a`,`f` as an
  `Identifier`, then stops at the first byte of `é` (`0xC3`) — a byte that is *also* not
  `isalpha`, not a digit, not whitespace, not a quote — and falls through to `op()`, which
  throws `unexpected character` with that single raw non-ASCII byte spliced literally into the
  message string. Two problems: (1) the identifier is silently truncated rather than the error
  pointing at the actual place the name became invalid, and (2) the diagnostic embeds a lone
  UTF-8 continuation byte, which is not valid UTF-8 on its own — printed to a terminal or
  captured by tooling it renders as a replacement glyph / mojibake, not something a user can
  act on (compare `KiritoError` messages elsewhere, which are always clean ASCII or valid
  UTF-8). The same failure mode applies to *any* non-ASCII byte reached by `op()`'s default
  case, including a bare embedded NUL — `unexpected character '\0'` — where the embedded NUL
  in the message string will truncate display in any consumer that treats it as a C-string
  (verified: printing the error via the CLI visibly cuts the message off after the opening
  quote).
- **Repro**: reproduced live —
  ```
  $ printf 'var caf\xc3\xa9 = 1\n' > t.ki && ki t.ki
  t.ki:1:8: error: unexpected character '<mangled byte, not valid UTF-8 alone>'
  ```
  and for NUL:
  ```
  $ printf 'var x = 1\x00\n' > t.ki && ki t.ki
  t.ki:1:10: error: unexpected character '        <nothing visible after this>
  ```
  (the message after the opening `'` is a literal NUL, invisible/truncating in most
  terminals/loggers).
- **Proposed fix**: two independent, minimal, contract-preserving options (either is enough; both
  are cheap):
  1. In `op()`'s default branch, render the offending byte legibly instead of splicing it raw —
     e.g. printable ASCII as-is, anything else as `\xHH` (mirrors the existing `\xHH` escape
     the lexer already emits/decodes elsewhere, so no new format is introduced). This alone
     fixes both the identifier-byte and NUL-byte manifestations without touching identifier
     semantics.
  2. Separately, decide whether `identifier()` should explicitly reject a non-ASCII byte with
     its own clear message (`"invalid character in identifier"`) rather than falling through to
     the generic unexpected-character path — this gives a *more specific* diagnostic but is
     optional if (1) is done, since the generic message becomes legible either way.
- **Proposed test**: a unit test asserting the exception `what()`/message for both `café` (an
  identifier containing a non-ASCII byte) and an embedded NUL byte is fully printable ASCII
  (e.g. matches `unexpected character '\\x..'` or similar), plus a `tools/tests/errors/*.ki` +
  `.experr` pair pinning the exact rendered text.

### A01-4: column tracking is byte-based, not code-point-based  [severity: LOW] [confidence: confirmed]
- **Location**: src/kirito/lexer.hpp:119-122 (`advance()` increments `col_` once per byte).
- **What**: `advance()` bumps `col_` by 1 per `char` consumed. For a line containing multi-byte
  UTF-8 (e.g. `café`), a diagnostic pointing at a token *after* the Unicode character reports a
  column counted in bytes, not in code points/columns — off by (bytes-per-char − 1) per
  preceding multi-byte character. This is consistent with the parser's own documented
  contract ("col counts BYTES", parser.hpp ~1087, used to map a token back to a byte offset for
  source-capture) so it is at least internally consistent, but it means any user-facing error
  on a line with non-ASCII text reports a column that doesn't match what a text editor would
  show, which is a real (if low-severity) diagnostics-quality gap.
- **Repro**: `var café2 = @` — the `@` is the 12th character (1-based) but its byte offset is
  13 (é is 2 bytes); confirmed the reported column reflects a byte count in a controlled
  side-test (the identifier-truncation bug in A01-3 masks this on the exact `café` case since
  the error fires before reaching `@`, but the byte-vs-codepoint divergence is directly visible
  by comparing `advance()`'s per-byte increment against any string containing a multi-byte
  character followed by more tokens on the same line).
- **Proposed fix**: N/A as a bug fix (changing this touches the source-capture byte-offset
  contract in parser.hpp and is a deliberate, load-bearing simplification) — this is recorded
  as a known, documented limitation, not something to change unilaterally. Flagging for
  triage/decision only.
- **Proposed test**: coverage gap — no test pins the current (byte-based) column behavior on a
  non-ASCII line, so a future accidental change either direction would go unnoticed. A small
  unit test asserting the reported column for a token following multi-byte UTF-8 text would at
  least pin today's behavior.

### A01-5: leading indentation at top level (before any statement) gives a generic parser error rather than "unexpected indent"  [severity: LOW] [confidence: confirmed]
- **Location**: src/kirito/lexer.hpp:142-181 (`handleIndentation`) — an indent from level 0 is
  always accepted and an `Indent` token emitted, regardless of whether a block-opening `:` was
  actually seen; that's the parser's job to reject, and it does so with a generic message.
- **What**: A `.ki` file whose very first (non-blank) line is indented emits an `Indent` token
  before any statement, which the parser's `parsePrimary`/statement dispatch doesn't recognize
  as a valid statement start, producing `expected an expression` rather than a specific
  `unexpected indent`-style message. This is a diagnostics-quality gap shared between the
  lexer (which happily accepts any indent increase syntactically) and the parser (which has no
  dedicated "indent not expected here" check) — noted here since it's the indentation
  subsystem's contract that's incomplete.
- **Repro**: reproduced live —
  ```
  $ printf '    var x = 1\n' > t.ki && ki t.ki
  t.ki:1:5: error: expected an expression
  ```
- **Proposed fix**: N/A for the lexer itself (the lexer's job — measuring indent consistently —
  is done correctly here; a real fix belongs in the parser: detect a stray leading `Indent`
  token and raise `unexpected indent` instead of falling into generic expression parsing).
  Recorded here because it's part of the indentation-diagnostics contract.
- **Proposed test**: `tools/tests/errors/*.ki` + `.experr` pinning `unexpected indent` (or
  whatever message is chosen) for a top-level indented first line.

### A01-6: DRY / minor — prefix recognition duplicated between `tryStringLiteral`'s probe and `stringLiteral`'s own consuming loop  [severity: LOW] [confidence: speculative]
- **Location**: src/kirito/lexer.hpp:273-285 (`tryStringLiteral`, one-of-each-flag probe loop)
  vs. 292-294 (`stringLiteral`, unconditional `while (peek() is r/R/f/F) advance();`).
- **What**: Two different loops recognize the same `r`/`f` prefix spelling: the probe (strict —
  at most one of each, in either order) decides *whether* to call `stringLiteral`, and once
  called, `stringLiteral` re-scans and consumes the same characters with a looser loop (no
  count limit) that relies on the probe having already validated the shape. This is safe today
  (verified: no input reaches `stringLiteral` with a prefix shape the probe wouldn't have
  validated, since the probe is the only caller for the prefixed path and the unprefixed path
  never has prefix characters to consume), but it is a duplicated recognition rule that could
  drift if either is edited independently (e.g. adding a third prefix letter in one place but
  not the other).
- **Proposed fix**: N/A — flagged for awareness only; a real consolidation (e.g. having
  `tryStringLiteral` pass the already-scanned prefix length to `stringLiteral` instead of
  re-scanning) is a bigger refactor than the drift risk currently justifies.
- **Proposed test**: N/A — coverage of the existing prefix combinations is already good via
  `test_string_literals.cpp`'s "prefixes are still valid identifiers" section.

---

## Summary

- **By severity**: 0 HIGH, 2 MED (A01-3, and arguably A01-3 alone), 4 LOW (A01-1, A01-2, A01-4,
  A01-5), 1 LOW/speculative DRY note (A01-6). Corrected count: **1 MED, 5 LOW**.
- **No crashes, hangs, or OOB reads found** despite adversarial probing: truncated escapes,
  lone backslash at raw-string EOF, unterminated triple strings with dangling quote runs,
  deeply repeated `{{`/`}}` f-string escapes (3000+ deep — the brace-matching scan is iterative,
  not recursive, so no stack risk at the lexer/tokenizer level), embedded NUL both in and out
  of string literals, huge decimal/hex literals (wraps two's-complement at parse time, by
  design), and mixed tab/space ambiguity at multiple nesting depths.
- **Top 3**:
  1. **A01-3 (MED, confirmed)** — non-ASCII/control bytes reaching `op()`'s default branch are
     spliced raw into the error message, producing an illegible or truncating diagnostic; a
     one-line `\xHH`-escaping fix in the error-message construction covers both the identifier
     case and the embedded-NUL case at once.
  2. **A01-1 (LOW, confirmed, carried over unfixed from v1.14)** — a leading UTF-8 BOM is a hard
     lex error; same class of Windows-friendliness fix as the existing CRLF normalization.
  3. **A01-2 (LOW, confirmed, carried over unfixed from v1.14)** — `1.e5` silently misparses as
     member access `(1).e5`, failing only at runtime with an unrelated attribute error instead
     of lexing as a float or failing cleanly at lex time.
