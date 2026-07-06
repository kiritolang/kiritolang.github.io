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
