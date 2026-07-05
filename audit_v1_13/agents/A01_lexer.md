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

(appended below as discovered)
