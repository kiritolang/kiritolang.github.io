# v1.15 audit — lexer & parser subsystem

Source: src/kirito/lexer.hpp, src/kirito/parser.hpp, src/kirito/common.hpp
Probe: ./build-debug/ki

## LOG
- Starting: reading all three source files deeply.

## FINDINGS

### F1 [MED] Trailing commas inconsistent: allowed in list/set/dict/value-seq but rejected in calls, params, multi-index
- where: src/kirito/parser.hpp:597 (call args), :721 (params), :617 (multi-index subscript)
- repro:
  - `io.print(1, 2,)`      -> error "expected an expression"
  - `Function(a, b,):`     -> error "expected a parameter name"
  - `t[0, 1,]`             -> error "expected an expression"
  - but `[1, 2,]`, `{1, 2,}`, `{1:2, 3:4,}`, `var a = 1, 2,` all accept the trailing comma
- actual: trailing comma is a parse error in call/param/index lists
- expected: consistent behavior; container literals + value-seq accept a trailing comma, so calls/params/index should too (Python allows all)
- fix idea: after consuming a comma in the call/param/index loops, `break` if the next token is the closer (RParen/RBracket) before parsing another element — mirror parseListLiteral's `if(!at(Comma)) break; advance();` shape.

### F2 [LOW] Stray INDENT token yields generic "expected an expression" instead of an "unexpected indent" diagnostic
- where: src/kirito/parser.hpp:845 (parsePrimary default) / lexer emits Indent that parser never handles at statement position
- repro:
  - `    var x = 1`  (file starts indented)  -> `1:5: error: expected an expression`
  - over-indented line inside a block:
    ```
    if True:
        io.print(1)
            io.print(2)
    ```
    -> `4:9: error: expected an expression`
- actual: misleading message pointing at the token after the indent
- expected: a clear "unexpected indent" IndentationError-style message
- fix idea: in parseStatement/parseProgram, detect a leading TokenType::Indent and throw "unexpected indent" with the indent token's span. Diagnostic-quality only; not a crash.

## LOG (examined / ruled out — all CLEAN unless noted)
- Numeric literals: over-64-bit decimal/hex wrap two's-complement (documented); `0x`/`0b` empty -> clean "invalid numeric literal"; `1e`/`1.2.3`/`.5`/`123abc` -> clean parse errors; huge (2M-digit) literal -> no crash, wraps. `1.`/`1.e5` require a digit after the dot (so `1.e5` mis-reads as `(1).e5` member access -> AttributeError; minor Python incompat, likely by-design). `0755` = decimal 755 (leading zero is NOT octal; octal is `0o`) — by design.
- Strings: unterminated single/triple, bad escape (`\q`), dangling backslash, `\x` short/non-hex, raw string ending in backslash — ALL clean lex errors with line/col. `\0` NUL embeds correctly (len counts it). `\xHH` emits U+00HH as UTF-8 (not raw byte) consistently in plain + f-strings.
- f-strings: `{{`/`}}` literal, nested quotes (`f"{d['k']}"`), `:format-spec` split is bracket-depth + quote aware (slice/dict colons not mistaken), single `}` and unmatched `{` -> clean errors. Escapes routed through the shared cooked-escape decoder (plain == f-string). Nested-placeholder spec `f"{x:{w}}"` -> clean "invalid format spec" (feature gap, errors cleanly). Deeply nested SAME-quote f-strings are capped by the (non-nested-quote-aware) lexer -> "unmatched '{'" (known limitation, no overflow). Triple-quoted multiline f-strings work.
- Indentation: tab/space ambiguity (wide=8 vs narrow=1 dual-measure) rejects mismatches; inconsistent dedent caught; CRLF + lone-CR normalized; blank/comment/trailing-whitespace lines handled; no dedent underflow (floor {0,0}). Mixed-on-same-line tab-after-space measured consistently.
- Deep-nesting stack guards: parens/lists/add-chain/not/neg/member/index/pow/ternary/and-or/if-block-nesting ALL throw "expression nested too deeply" (DepthGuard + ChainDepth), no crash. kMaxParseDepth 2000 (250 sanitizer).
- No infinite loops (every lexer branch + string/indent scan makes progress); no advance-past-EOF (expect never advances on non-matching EOF; peekAt clamps).
- Invalid assignment targets (literal/call/binop), return/break/continue-outside-context, `*x` splat in call/print, empty subscript `a[]`, two starred targets -> all clean errors, no crash.
- Soft keywords `case`/`default` usable as ordinary names + `default` parameter; keyword-as-member-name (`s.discard`, `obj.type`) works; `obj."str"` correctly rejected.
- Column/line in diagnostics accurate (tab counted as 1 col in positions; ambiguity errors pinned to col 1 — minor). Huge identifier (2M chars) parses without crash.
- Chained comparison `1<2<3` is left-assoc `(1<2)<3` -> throws "Bool does not support this binary operator" (NO Python chaining; errors rather than silently misleading) — by-design, informational.

## SUMMARY
Lexer/parser are robust: no crashes, hangs, memory-safety, or resource-exhaustion issues found under
a broad adversarial battery. 2 confirmed findings, both low-impact: F1 (MED) trailing-comma
inconsistency across call/param/index vs container literals; F2 (LOW) stray-INDENT diagnostic quality.
