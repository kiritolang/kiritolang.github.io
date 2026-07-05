# A01 — Lexer / Tokenizer Audit

**Scope:** `src/kirito/lexer.hpp` (complete). Read-only static analysis.
**Auditor:** A01. **Date:** 2026-07-05.

## Lexer surface enumerated

- Token kinds: literals (Integer, Float, String, FString, Identifier), keywords (var/True/False/None/if/elif/else/while/break/continue/and/or/not/Function/return/for/in/try/catch/finally/throw/as/class/with/pass/todo/assert/discard/switch), operators (+ - * / // % ** -> = == != < <= > >= ( ) [ ] { } : , .), layout (Newline, Indent, Dedent, EOF).
- Soft keywords: `case`/`default` lexed as identifiers (recognized in parser).
- String spellings: single/double quote, triple `'''`/`"""`, prefixes r/R, f/F, rf/fr (either order, case-insensitive, each once).
- Cooked escapes: \n \t \r \0 \\ \" \' \xHH. f-strings & raw keep escapes verbatim.
- Numeric: decimal, 0x/0X hex, 0o/0O octal, 0b/0B binary, float with `.` and scientific `e`/`E` +/- exponent. Text kept; parser decodes base/value.
- Comments: `#` to end of line.
- Indentation: dual-measure wide(tab=8)/narrow(tab=1); ambiguity rejected. parenDepth suppresses newlines/indentation inside ()/[]/{}.
- Newline normalization: CRLF and lone CR → LF up front.
- Sub-lexer ctor seeds line/col for f-string embedded exprs.

---

## Findings

### A01-1: f-string `\xHH` with HH >= 0x80 emits a raw byte, not the UTF-8 code point
- severity: Medium
- location: src/kirito/parser.hpp:986 (f-string escape decode) vs src/kirito/lexer.hpp:350-358 (plain-string `decodeEscape`)
- category: bug
- description: The lexer decodes a plain-string `\xHH` as Unicode code point U+00HH and emits its UTF-8 encoding (1 byte for HH<0x80, 2 bytes for HH>=0x80) — see the A01-1 comment at lexer.hpp:344-349. The f-string escape decoder in `parseFString` instead does `lit += static_cast<char>(hi*16 + lo)`, i.e. a single RAW byte, even for HH>=0x80. So `f"\xE9"` yields a String whose bytes are `{0xE9}` — a lone lead/continuation byte that is invalid UTF-8 — whereas `"\xE9"` yields the valid two-byte UTF-8 `{0xC3,0xA9}` (U+00E9). The two spellings produce non-equal, structurally different Strings, and the f-string result violates the String type's "sequence of code points stored as UTF-8" invariant, so downstream code-point `len`/indexing/iteration operate on malformed UTF-8.
- failure scenario: `f"\xff"` → String bytes `{0xFF}` (malformed); `"\xff"` → `{0xC3,0xBF}`. `f"\xff" == "\xff"` is False; `len(f"\xC3\x28")` misbehaves exactly like the bug the plain-string path was fixed for.
- proposed test: assert `f"\xff" == "\xff"` and `len(f"\xe9") == 1` and `f"\xe9" == "é"` (add to test_string_literals.cpp escape table so the f-string flavour is checked with a high byte, not only `\x41`).
- proposed fix: have `parseFString` route `\xHH` through the same code-point→UTF-8 emission the lexer's `decodeEscape` uses (extract that emission into a shared helper — see A01-3).
- confidence: high

### A01-2: f-string silently drops the backslash on an unknown escape; plain strings throw
- severity: Medium
- location: src/kirito/parser.hpp:998 (`default: lit += e;`) vs src/kirito/lexer.hpp:369-372 (`default: throw ... "invalid escape"`)
- category: bug
- description: CLAUDE.md and the lexer contract state "a bad escape ... is a clear lex error." That holds for plain/cooked strings (`decodeEscape` throws `invalid escape '\q'`), but the f-string decoder's `default` arm silently appends the escape char and drops the backslash, so `f"\q"` becomes `"q"` with no error. This is an undocumented, silent divergence: the same source fragment lexes differently depending on the `f` prefix, and a typo'd escape passes unnoticed inside an f-string.
- failure scenario: `f"a\qb"` → `"aqb"` (no error); `"a\qb"` → throws `invalid escape '\q'`. Inconsistent and violates the documented "bad escape is a clear lex error" rule.
- proposed test: assert that importing `var s = f"\q"` raises a lex error with `invalid escape` (mirror errors/bad_escape.ki for the f-string flavour); assert `f"\d"` behaves identically to `"\d"`.
- proposed fix: make the f-string `default` arm throw the same `invalid escape '\<e>'` error as `decodeEscape` (again best done by sharing the decoder — A01-3).
- confidence: high

### A01-3: escape-decoding logic duplicated between lexer `decodeEscape` and parser `parseFString`
- severity: dry
- location: src/kirito/lexer.hpp:334-376 and src/kirito/parser.hpp:976-1000
- category: dry
- description: The set of cooked escapes (`\n \t \r \0 \\ \" \'` + `\xHH`) is implemented twice with subtly different semantics — the divergences in A01-1 (raw byte vs UTF-8) and A01-2 (silent-drop vs throw) are the direct consequence. `hexDigitValue` is already the shared hex helper (common.hpp:93); the escape *table + code-point emission* should likewise be one function consumed by both the plain-string lexer and the f-string literal-part decoder, so the two can never drift again.
- proposed fix: extract a `decodeCookedEscape(text, i, out)` free function (in common.hpp or lexer.hpp) returning the decoded bytes + advance count, throwing on a bad escape, and call it from both sites.
- confidence: high

### A01-4: dead `'\r'` handling — unreachable after `normalizeNewlines`
- severity: Low
- location: src/kirito/lexer.hpp:78 (main loop `c == '\r'` skip) and any `\r` handling
- category: dry
- description: `normalizeNewlines` (called in every constructor) rewrites every `\r` (CRLF and lone CR) to `\n` before lexing, so `src_` never contains a `\r`. The `c == ' ' || c == '\t' || c == '\r'` skip branch's `\r` case is therefore dead code. Harmless but misleading — it suggests raw CR can reach the main loop, which it cannot.
- proposed fix: drop the `|| c == '\r'` from the skip predicate (or add a comment noting it is defensive/unreachable). No test needed; it is a clarity cleanup.
- confidence: high

### A01-5: cooked string ending in a lone backslash at EOF reports "invalid escape" (with an embedded NUL) instead of "unterminated string"
- severity: Low
- location: src/kirito/lexer.hpp:306-321 (loop) → 334-372 (`decodeEscape`)
- category: weak-spot
- description: For a cooked single/double string whose last character before EOF is a backslash (e.g. source `"abc\` with no closing quote), the loop is not at `atClose` and not `atEnd` (the backslash byte exists), so it calls `decodeEscape`, which `advance()`s past the backslash and then reads `peek()` == `'\0'` (the end-of-input sentinel). `'\0'` is not `x` and hits the `default` arm, throwing `invalid escape '\<NUL>'` — a message containing a literal NUL byte — rather than the correct `unterminated string`. The user-facing diagnostic is wrong and contains a control byte.
- failure scenario: `var s = "abc\` (file ends immediately after the backslash) → `invalid escape '\<NUL>'` instead of `unterminated string`.
- proposed test: errors/*.ki with body `var s = "abc\` (trailing backslash, no newline, no close) expecting `unterminated string`.
- proposed fix: in `decodeEscape`, check `atEnd()` right after consuming the backslash and throw the unterminated-string error (or have the caller detect a trailing backslash before dispatching to `decodeEscape`).
- confidence: high

### A01-6: locale-dependent character classification for identifiers/whitespace
- severity: Low
- location: src/kirito/lexer.hpp:90,96,202,227 (`std::isdigit`/`isalpha`/`isalnum`), and `std::isxdigit` at 191
- category: weak-spot
- description: The lexer uses the locale-sensitive `<cctype>` classifiers. For bytes >= 0x80, `std::isalpha`/`isalnum` results depend on the host's active C locale (the embedder may have called `setlocale`). In the default `C` locale, high bytes classify as non-alpha (so a UTF-8 identifier byte falls through to `op()` → clean `unexpected character` error), but under a locale where high bytes are alpha, stray non-ASCII bytes could be silently accepted into identifiers — making tokenization host-locale dependent. Kirito otherwise takes care to be host-independent (VM-encapsulated, no globals); locale is an unmanaged global input.
- failure scenario: Embedder sets a Latin-1 locale; a source with a byte like 0xE9 mid-identifier lexes into a valid identifier on that host but a lex error on a default-locale host — non-portable behavior for identical source.
- proposed test: (documentation/robustness) a unit test that a non-ASCII byte in identifier position throws `unexpected character` regardless of locale; consider replacing classifiers with explicit ASCII-range checks.
- proposed fix: use explicit ASCII predicates (`c>='0'&&c<='9'`, `(c|32)>='a'&&(c|32)<='z'`, etc.) so lexing is locale-invariant. Digit/xdigit for ASCII are stable, but alpha/alnum on high bytes are the risk.
- confidence: medium

### A01-7: coverage gap — f-string escape decoding is untested for high bytes and unknown escapes (the buggy paths)
- severity: coverage-gap
- location: tools/tests/unit/test_string_literals.cpp:80-99 (escape table)
- category: coverage-gap
- description: The escape-table test exercises f-strings only with `\x41` (ASCII, HH<0x80) and the standard `\n\t\r\\\0` set — never a high-byte `\xHH` (which would expose A01-1) nor an unknown escape like `\q` in an f-string (which would expose A01-2). The plain-string high byte is covered (r8_language.ki:353 `len("\xff")==1`), but the f-string flavour of the same case is not, so both A01-1 and A01-2 sit in a blind spot. No `.ki` script or unit test compares `f"\xNN"` against `"\xNN"` for NN>=0x80.
- proposed test: add high-byte and unknown-escape rows to the f-string flavour assertions (see A01-1/A01-2 proposed tests).
- confidence: high

### A01-8: coverage gap — form-feed/vertical-tab, NUL byte, and non-ASCII bytes in source are untested edges
- severity: coverage-gap
- location: src/kirito/lexer.hpp (main loop 71-100, handleIndentation 137-176)
- category: coverage-gap
- description: No test covers: (a) a form-feed (0x0C) or vertical-tab (0x0B) at line start or in content — these are not counted as indentation whitespace and are not skipped in the main loop, so they reach `op()` → `unexpected character`; behavior is plausibly correct but unverified and differs from Python (which treats `\f` specially). (b) A genuine NUL byte (0x00) embedded in source (outside a string) — reaches `op()` default, producing an `unexpected character '<NUL>'` message with a control byte. (c) A non-ASCII byte in identifier/operator position — throws `unexpected character` but is untested (and see A01-6). These all currently throw rather than crash, but none is pinned by a test.
- proposed test: errors/*.ki (or unit) feeding a `\f` in content, a NUL byte, and a UTF-8 letter as an identifier, each asserting a clean thrown lex error (no crash, sane message).
- confidence: high

### A01-9: coverage gap — several lexer error messages exercised only by CHECK_THROWS, not by asserted text
- severity: coverage-gap
- location: tools/tests/unit/test_indent.cpp:46-53; src/kirito/lexer.hpp:172 ("inconsistent dedent")
- category: coverage-gap
- description: The distinct `inconsistent dedent` message (lexer.hpp:172, the dedent-matches-no-enclosing-level path) is triggered by test_indent.cpp's `"if x:\n    y\n  z\n"` case but only via `CHECK_THROWS` — the specific message is never asserted, so a regression that swapped it for the wrong diagnostic (e.g. `inconsistent use of tabs`) would pass. Same-wide/different-narrow ambiguity (lexer.hpp:161) and the `>`-branch narrow-check ambiguity (line 163) are likewise thrown-but-not-message-verified. Only `inconsistent use of tabs and spaces` has a message-asserting error test (surf_lex_16, mixed_tabs_spaces).
- proposed test: convert the indent CHECK_THROWS cases to assert the exact message, or add errors/*.ki with `.experr` for `inconsistent dedent` and the same-wide/different-narrow ambiguity.
- confidence: high

### A01-10: coverage gap — position/column tracking has almost no direct assertions
- severity: coverage-gap
- location: src/kirito/lexer.hpp:114-117 (advance), make() spans; tools/tests/unit/test_lexer.cpp:37-38
- category: coverage-gap
- description: Only one test asserts a token's line/col (test_lexer.cpp:37-38, the first token of a single line at 1:1). There is no test that verifies column advancement across a multi-byte UTF-8 code point in a string, across a tab, after a comment, on the second line, or on the Newline/Indent/Dedent tokens. Off-by-one regressions in `advance`/`col_` would go undetected. Note also that `advance()` increments `col_` per BYTE, so a multi-byte UTF-8 char in a line advances the column by its byte count, not by 1 code point — columns reported for content after a non-ASCII char are byte-offsets, not character-offsets (an accuracy quirk worth a test to pin the intended contract).
- proposed test: a lexer unit test asserting `.span.line/.col` for a spread of tokens on lines 2+, after tabs, after comments, and after a non-ASCII string, documenting whether columns are byte- or code-point-based.
- confidence: medium

### A01-11: `inconsistent dedent` / ambiguity error spans always point at column 1
- severity: Low
- location: src/kirito/lexer.hpp:155-156, 172 (`SourceSpan{line_, 1, 0}`)
- category: weak-spot
- description: The indentation errors hard-code column 1 (`SourceSpan{line_, 1, 0}`) rather than the column where the mismatch was detected, and the token `make()` always sets span width 0 (line 127). Diagnostics for indentation problems therefore cannot point at the offending whitespace column. Cosmetic, but the CLAUDE.md contract emphasizes "line and column and a message a user can act on."
- proposed fix: pass the measured column (or the narrow width) into the span; low priority.
- confidence: high

---

## Notes on things checked and found OK
- Integer literal overflow: decimal/hex/oct/bin decode in unsigned 64-bit space and wrap two's-complement (parser.hpp:1016 `intLiteral`); `0xFFFFFFFFFFFFFFFF == -1`, over-64-bit wraps, `9223372036854775808 == INT64_MIN` — never throws. Covered by r4_numbers.ki:38-45, r8_language.ki:72-74. No overflow/UB.
- Float literal overflow/underflow: `parseDouble` (common.hpp:78) accepts subnormals (`5e-324`), overflow→+inf via caught out_of_range (parser.hpp:793-798). No crash on huge/tiny literals.
- Base-prefix with no digits (`0x`,`0o`,`0b`): throws `invalid numeric literal` — surf_lex_08/09/10.
- `5.` / `.5` bare-dot floats and `1_000` digit separators: rejected — r8_language.ki:79-83.
- Nested f-strings: bounded — `parseEmbedded` seeds `exprDepth_+1` into the sub-parser (parser.hpp:898), so deep f-string nesting trips the DepthGuard rather than overflowing the native stack.
- Quote-aware f-string brace/`:`-spec scanning: handles `f"{d['k']}"`, `f"{'a:b'}"`, `{{`/`}}`, slices `x[1:2]`, nested dict `{1:2}` (parser.hpp:928-969); tested in test_fstring.cpp and test_string_literals.cpp.
- Unterminated single/triple/raw strings, raw-string-ending-in-backslash: throw cleanly — surf_lex_01/02/03/04/15, unterminated_raw_fstring.
- `\x` short/non-hex, unknown escape (plain), single `}`, empty `{}`, unmatched `{`: all covered by surf_lex_05/06/07/13/14 and fstring_unmatched_brace.
- CRLF / lone-CR normalization: normalizeNewlines collapses up front; test_lexer.cpp:69 (`"1\r2\r"`). Note: this also normalizes CR inside string literals to LF (documented universal-newlines behavior, not a bug).
- No native recursion in the lexer itself (iterative); no unbounded native stack growth. `parenDepth_`/`indent_` growth is bounded by input size only.
- No infinite-loop path found in the tokenize/handleIndentation state machine (every branch makes progress; blank-line/comment handling terminates at EOF).
