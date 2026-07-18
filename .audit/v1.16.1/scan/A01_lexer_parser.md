# A01 — Lexer + Parser + f-strings audit (v1.16.1)

Scope: src/kirito/lexer.hpp, parser.hpp (+ ast.hpp for shapes).

## Findings

## Coverage notes

### F01-1 [Med] Comparison operators are left-associative, not chained — `1 == 1 == 1` is `False`, `1 < 2 < 3` throws
- parser.hpp:518-547 (parseComparison) — the `while(true)` loop folds comparisons LEFT-associatively into a chain of `BinaryExpr`, so `a OP b OP c` becomes `(a OP b) OP c`. Because a comparison yields a `Bool`, the second operator then compares a Bool against the third operand.
- Trigger: `1 == 1 == 1` -> prints `False` (evaluates `(1==1)==1` = `True==1` = False). `1 < 2 < 3` -> runtime `type 'Bool' does not support this binary operator`. `1 < 2 == True` -> `True` (accidental). All confirmed with build-debug/ki.
- Why it's a weak spot: Kirito is heavily Python-modelled; users will write `0 <= i < n` and `a == b == c` expecting Python chaining. Instead they get a silently-wrong Bool result (`==`/`!=`) or a cryptic runtime type error (`<`/`>`). No spec text sanctions left-assoc comparison.
- Fix idea: make comparison NON-associative — after parsing one `left OP right`, if another comparison operator follows, throw a clear parse error ("chained comparison is not supported; use `and`") — OR implement real Python chaining (desugar `a<b<c` to `a<b and b<c` with single-eval of b). Minimal/safe: reject the second comparator at parse time.
- Test to add: `.ki` error test `1 < 2 < 3` and `1 == 1 == 1`; assert clear diagnostic (or chained semantics if implemented).
- Verified-real: yes — confirmed runtime output.

## Confirmed-robust (verified, NOT bugs — regression-worthy)
- Depth guard: 5000 nested `(`/`[`/`{`, 20000-long `+`/index chains all throw "expression nested too deeply" (~2000 cap), no native overflow. parser.hpp:419-450.
- Numeric: `0x`/`0o8`/`0b2`/`0xG` bad-digit -> clean "invalid numeric literal"; `1.e5` float; `1.compare(1.0)` member (no paren needed); INT64 two's-complement wrap (`9223372036854775808`->MIN, `0xFFFFFFFFFFFFFFFF`->-1, giant decimal wraps); `1e400`->inf; `5e-324` subnormal OK (parseDouble, not stod).
- Strings: unterminated single/triple, single-line crossing newline, raw ending in backslash, bad escape `\q`, `\xZZ`, `\x41`, `\0` (NUL is len-counted) — all clean lexer errors / correct decode.
- f-strings: `{{`/`}}`, single-quoted key in double-quoted f-string, `{expr:.2f}` spec, colon-in-slice/dict quote+bracket-aware, `\x41`/`\q` route through shared cooked-escape decoder (parity with plain strings), raw `rf"\n{1}"`. Lone `}` and unmatched `{` clean errors.
- Indentation: tab-vs-8-space ambiguity rejected (tab=1/tab=8 disagreement), bad dedent column -> "inconsistent dedent". Leading BOM stripped, mid-file BOM -> error. CRLF and lone-CR normalized.
- Unpack: two starred targets rejected; starred first/middle/last OK.
- Parse-time rejects: return/break/continue outside fn/loop, duplicate param (hard error), assign-to-literal/call (invalid target), empty subscript `xs[]`, two `default:` in switch. `case`/`default` usable as names (soft keywords).
- Runtime-deferred (catchable) throws: positional-after-keyword, duplicate keyword arg, duplicate switch case value.
- Serialization: Function and class instance dump/loads round-trip (captureSource verbatim slice) works.

### F01-2 [Med] `var a, = x` and `for x, in xs` drop the trailing comma — no 1-element unpack, silent count-mismatch swallow (inconsistent with `a, = x`)
- parser.hpp:302-314 (parseTargetNameList, used by `var` and `for`) — after reading a name it consumes a following `,` and, seeing `Assign`/`KwIn`, `break`s treating it as a trailing comma; the single-target-with-trailing-comma tuple context is lost, so the decl collapses to a plain single-name bind. The general assignment path (parseStatement:133-145) instead builds a 1-element `TupleExpr`, preserving unpack semantics.
- Trigger (all confirmed): `var a, = [1]` -> a == `[1]` (should be `1`); `var a, = [1,2]` -> a == `[1,2]` (no error); `for x, in [[1],[2]]` -> x == `[1]`,`[2]` (should be `1`,`2`). But the assign form `a, = [1,2]` correctly throws "expected 1 values to unpack, got 2", and `a, = [99]` -> a == 99.
- Why a bug: identical trailing-comma syntax means "unpack as 1-tuple" for bare assignment but "bind whole" for `var`/`for` — a silent semantic split, and the var/for forms silently swallow a count mismatch the assign form catches.
- Fix idea: in parseTargetNameList, if a trailing comma was seen (even with a single name), record it so the codegen forces 1-element tuple-unpack context (a `bool sawTrailingComma` / treat single-name+comma as an unpack of arity 1). Or reject `var a,` /`for x,` as needless. Keep it consistent with the assignment path.
- Test to add: `.ki` tests asserting `var a, = [1]` gives `a==1` and `var a, = [1,2]` / `for x, in [[1,2]]` error on count.
- Verified-real: yes.

### F01-3 [Low] Escaped quote inside an f-string `{expr}` -> confusing "unmatched '{'" instead of a clear diagnostic
- parser.hpp:1004-1019 (parseFString matching-brace scan) — the scanner tracks a string state but only honors backslash-escapes WHILE already inside a quote (`if (quote) { if (esc)... }`). Outside a quote a `\"` is seen as an unescaped `"` that OPENS a string, so the lexer-preserved escaped quote inside an f-string expression flips quote tracking and the closing `}` is swallowed as "string data" -> depth never returns to 0.
- Trigger: `f"{d[\"k\"]}"` -> `unmatched '{' in f-string` (the lexer legitimately keeps `\"` verbatim in an f-string's raw text; the brace scanner then mis-reads it).
- Why minor: an escaped quote inside an f-string expression can never actually work anyway (the sub-lexer would reject the stray `\` too — Python <3.12 forbids backslashes in f-string expressions outright); the fault is purely the misleading message. The documented idiom `f"{d['k']}"` (single quotes) works. Same class applies to the `:spec` split scan (parser.hpp:1029-1041).
- Fix idea: either detect a backslash inside an f-string `{...}` and throw "backslash is not allowed inside an f-string expression", or make the brace scanner honor `\` outside quotes too (skip the next char) so the error becomes the sub-lexer's clean one.
- Test to add: error test `f"{d[\"k\"]}"` expecting a clear "backslash in f-string expression" diagnostic.
- Verified-real: yes (message confirmed; it is a diagnostic-quality issue, not a wrong-result).

### F01-4 [Low] Minor diagnostic gaps
- parser.hpp:92-98 — `todo f"msg"` (f-string reminder) is rejected with "expected end of statement" because only a `String` token is accepted as the message; an f-string reminder is a natural thing to write. Same for the inline `todo` (parser.hpp:805-810). Low: use a plain string.
- lexer.hpp:156 / program start — a leading-indented first line yields "expected an expression" (the parser meets a stray INDENT) rather than Python's clearer "unexpected indent". Low diagnostic nit.
- Verified-real: yes (both messages confirmed).

## Coverage notes (untested / under-tested surface for phase 2)
- Chained/associativity of comparison operators (F01-1) — no test asserts what `1<2<3` / `a==b==c` do; add one either way.
- `var`/`for` single-target trailing comma (F01-2) — no test covers `var a, = ...` or `for x, in ...`.
- Nested f-string format spec `f"{x:{w}}"` — unsupported (throws "invalid format spec '{w}'"); no test documents this boundary.
- Numeric separators (`1_000`) — unsupported; `1_000` lexes as `1` then `_000` (a name). No test documents.
- Leading-dot floats (`.5`) — unsupported (lexes as Dot then Integer); no test.
- `1.e5` / `1.e` boundary, `1e`/`1e+` (no-digit exponent falls back to Integer+identifier) — thinly covered; add explicit lexer tests.
- Non-ASCII identifiers rejected (isalnum ASCII-only), Unicode in strings OK, non-ASCII operator byte shown as `\xHH` — confirm with tests.
- Escaped-quote-in-f-string-expr diagnostic (F01-3); backslash-in-f-string-expr generally.
- captureSource/serialization edge cases: inline fn body spanning brackets, trailing comment after fn, default referencing earlier param, subclass instance — all currently WORK (verified) but pin with round-trip tests if not already present.
- Triple-quoted string containing 1-2 inner quotes (`"""a""b"""`) — works; add a lexer test.
- Deeply nested VALID structures (~1900) run + tear down safely at -O0; a regression test near the depth cap would guard the teardown path.
- `switch`/`case`/`default` soft-keyword-as-name (`var default = 5`, `var case = 7`) — works; add tests.
