# A01 — lexer + parser (v1.15.1)

Scope: `src/kirito/lexer.hpp`, `src/kirito/parser.hpp`, `src/kirito/ast.hpp`.
Binary probed: `./build-debug/ki` (no builds run, per round rule).

Status: IN PROGRESS

---

### A01-1: an inline `Function` literal that relies on an enclosing bracket's line-continuation captures un-reparsable source → silently unserializable  [severity: MED] [confidence: confirmed]

- **Location**: `src/kirito/parser.hpp:1122` (`captureSource`) / `parser.hpp:767` (`parseFunction`), with
  `lexer.hpp:95-102` (newline suppression at `parenDepth_ > 0`) as the root cause.
- **What**: Inside `(`/`[`/`{` the lexer emits **no `Newline`/`Indent` tokens** (line continuation). So a
  `Function` literal written inside a call/list can spread its **inline** body over several physical
  lines and parse fine. `captureSource` then slices those lines **verbatim, without the enclosing
  bracket**. serde re-parses that text standalone (`stdlib_serde.hpp:363`,
  `vm.evalIn("var __deser = " + source)`) at **paren depth 0**, where the newline is now significant —
  so the re-parse fails. This breaks the v1.15 contract that "`Function` … VALUES are serializable BY
  DEFAULT, self-contained": the function runs correctly, but serializing it throws.
  Two distinct triggers, both confirmed:
  1. body continues on the next line (natural wrapped formatting) → `expected an expression`
  2. body starts at column 1 on the next line → `expected an indented block`
- **Repro 1** (`/tmp/a01/p.ki`):
  ```
  var io = import("io")
  var ser = import("serialize")
  var mk = Function(f): return f
  var k = mk(Function(p): return p[0] * 1000 +
      p[1])
  io.print(k([2,5]))
  io.print("SRC>>>" + ser.dumps(k) + "<<<")
  var g = ser.loads(ser.dumps(k))
  io.print("roundtrip:", g([2,5]))
  ```
  Real output:
  ```
  2005
  SRC>>>KSER1 1 U 42 Function(p): return p[0] * 1000 +
      p[1] 0 0<<<
  Traceback (most recent call last):
    File "/tmp/a01/p.ki", line 8, in <module>
  /tmp/a01/p.ki:8:18: error: cannot deserialize function: expected an expression
  ```
- **Repro 2**:
  ```
  var io = import("io")
  var ser = import("serialize")
  var xs = [Function():
  return 1
  ]
  var g = ser.loads(ser.dumps(xs[0]))
  io.print(g())
  ```
  Real output:
  ```
  /tmp/a01/p.ki:7:18: error: cannot deserialize function: expected an indented block
  ```
  (The same function **runs** fine — `io.print(xs[0]())` prints `1`, and the captured source is
  `Function():\nreturn 1`.)
- **Impact**: anyone who `dump`s/`serialize`s a callback, or stores one in a `parallel` Queue, where the
  literal was written across lines inside a call/list. The code works; the failure surfaces only at
  serialize time, far from the cause, with a message that names neither the function nor the layout.
- **Proposed fix**: in `parseFunction`, when `node->inlineBody` is true, wrap the captured text in
  parentheses: `node->source = "(" + captureSource(startTok) + ")"`. Restoring the paren restores the
  newline suppression the literal was parsed under, so the re-parse sees exactly the token stream the
  original did. Safe superset:
  - a **block**-bodied literal must *not* be wrapped (its `Indent`s would be suppressed) — and a
    block-bodied literal can never occur inside brackets anyway, because at `parenDepth_ > 0`
    `at(TokenType::Newline)` is false so `parseFunction` always takes the inline branch (verified:
    `[Function():\n    return 1\n]` yields an inline-bodied `[<function>]`);
  - an inline literal already at depth 0 re-parses identically wrapped or not.
  Verified live — all three shapes re-parse correctly when wrapped:
  ```
  var __deser = (Function(p): return p[0] * 1000 +
      p[1])                      # -> 2005
  var __d2 = (Function(): return 1)          # -> 1
  var __d3 = (Function():
  return 1)                                   # -> 1
  ```
  Risks no documented contract (`FunctionExpr::source` is internal to serde; nothing else reads it —
  `parallel.spawn` resolves by file span, not by `source`).
- **Proposed test**: `tools/tests/scripts/` serde script (e.g. alongside the existing function-serde
  cases) — `var k = mk(Function(p): return p[0] +\n    p[1])` then assert
  `ser.loads(ser.dumps(k))([1,2]) == 3`; plus the column-1 variant. Also a C++ unit test in
  `test_parser*.cpp` asserting the captured `FunctionExpr::source` of a bracket-continued inline literal
  re-parses. Must be verified to FAIL on the current build (it does — see repros).

---

### A01-2: an error inside an f-string reports the f-string TOKEN's line/col, not the placeholder's — the column is wrong for any non-leading placeholder, and the LINE is wrong in a triple-quoted f-string  [severity: LOW] [confidence: confirmed]

- **Location**: `src/kirito/parser.hpp:936` (`parseEmbedded`: `Lexer lex(code, span.line, span.col);`),
  `lexer.hpp:45` (the seeded `Lexer` ctor).
- **What**: the v1.15 A02-2 fix seeds the f-string's sub-lexer with the **f-string token's** line/col so
  inner AST spans are "absolute to the enclosing source file". But the seed ignores (a) the prefix +
  opening quotes (`f"`, `rf"""`, …) and (b) the offset of the `{` **within** the literal. So the seed is
  only correct when the placeholder is the literal's very first character. The comment claims errors
  "report the real file location" — they do not.
  - single-line f-string: the reported **column** points into the literal text, not at the expression;
  - triple-quoted f-string: the reported **line** is the f-string's opening line, so a placeholder on a
    later physical line is reported on the wrong line — and that line is what the **traceback** prints.
- **Repro** (`p.ki`):
  ```
  var io = import("io")
  var d = {}
  io.print(f"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa{d[1]}")
  ```
  Real output — the `[` is genuinely at **column 44**:
  ```
    File "p.ki", line 3, in <module>
  p.ki:3:11: error: key not found: 1
  ```
  And the multi-line case — the placeholder is on **physical line 4**:
  ```
  var io = import("io")
  var d = {}
  var s = f"""line1
  line2 {d[1]}"""
  ```
  Real output:
  ```
    File "p.ki", line 3, in <module>
  p.ki:3:10: error: key not found: 1
  ```
  (Parse errors are unaffected — `parseEmbedded` re-anchors them to `span` in its `catch`. Only
  resolver/runtime errors carry the drifted sub-spans.)
- **Impact**: diagnostics only, no wrong behaviour. A user debugging a runtime error in a long or
  multi-line f-string is pointed at the wrong column, and at the wrong line in a traceback.
- **Proposed fix**: the parser already owns `source_` + `lineStart_`, so it can compute the exact
  location: from `byteOf(t.span)`, skip the `r/R/f/F` prefix letters, skip the opening quote (1 or 3),
  and add the `{`-relative index `i + 1` that `parseFString` already has; map that byte back to
  (line, col) with `lineStart_` and seed the sub-lexer with it. Fall back to the current behaviour when
  `source_` is empty (a nested sub-parse). Alternatively, if exactness isn't wanted, seed with
  `span.line, span.col` **and** re-anchor runtime spans too, so at least the reported location is
  consistently "the f-string" rather than an arbitrary interior column. Breaks no documented contract.
- **Proposed test**: `tools/tests/unit/test_fstring.cpp` — assert the span of the inner expression of
  `f"aaaa{d[1]}"` has the column of the real `[`; plus a `tools/tests/errors/` case pinning the
  reported `line:col` for a triple-quoted f-string with a placeholder on a later line.

### A01-3: CLAUDE.md misdescribes `positional argument follows keyword argument` as a compiler diagnostic; it is a deferred, catchable runtime throw  [severity: LOW] [confidence: confirmed]

- **Location**: `CLAUDE.md` (Architecture → "Execution engine: bytecode"), vs `src/kirito/compiler.hpp:652-666`.
- **What**: CLAUDE.md states *"A genuine program error the compiler finds (deep nest, invalid assignment
  target, positional-after-keyword) is thrown as a `KiritoError`, like a parser diagnostic."* It is not:
  `Compiler::visit(const ast::CallExpr&)` deliberately **compiles a `Throw`** for this case, so it fires
  only when the call is reached and it **is catchable**. `docs/pages/12-exceptions.md:84-86` documents
  the real behaviour correctly ("Two compiler checks — `positional argument follows keyword argument`
  and `duplicate switch case value` — are deferred to the point the code is reached, so they *are*
  catchable"), so CLAUDE.md is the stale copy. Deep-nest and invalid-assignment-target ARE real
  compile errors, so the sentence is only wrong about the third item.
- **Repro**:
  ```
  var io = import("io")
  var f = Function(a,b):
      return a
  var g = Function():
      return f(a=1, 2)      # never called
  io.print("reached")
  ```
  Real output: `reached` — i.e. no compile error. And it is catchable:
  ```
  try:
      io.print(f(a=1, 2))
  catch as e:
      io.print("caught:", e)
  ```
  → `caught: positional argument follows keyword argument`
- **Impact**: doc-only; misleads the next auditor/maintainer into "fixing" a deliberate design choice
  (which `tools/tests/errors/surf_parse_13` + `tools/tests/scripts/r10_language.ki:553` pin).
- **Proposed fix**: drop `positional-after-keyword` from that CLAUDE.md list, or reword to match
  `docs/pages/12-exceptions.md`. No code change; no contract at risk.
- **Proposed test**: none needed (`surf_parse_13.experr` + `r10_language.ki:553` already pin the
  behaviour); this is a documentation correction only.

### A01-4: an inline function body rejects `discard`, but the analyzer tells you to use `discard` there — the recommended fix does not parse  [severity: MED] [confidence: confirmed]

- **Location**: `src/kirito/parser.hpp:773-817` (`parseInlineStatement`) — its `switch` handles only
  `KwReturn`, `KwPass`, `KwTodo`, `KwThrow`, and a `default:` that runs `parseExpr()` (assignment /
  bare expression). `KwDiscard` and `KwAssert` are absent, so they fall into `default:` and
  `parseExpr()` immediately fails on the keyword token.
- **What**: `discard EXPR` is *the* language mechanism for "call this for its side effect, ignore the
  result" — exactly what an inline lambda is for. The analyzer emits
  `result of expression is unused; prefix with 'discard' to ignore it intentionally` **inside inline
  bodies**, but doing what it says is a parse error. The advice is unfollowable, and the resulting
  diagnostic (`expected an expression`) names neither `discard` nor the inline-body restriction. The
  parser's own comment at `parser.hpp:761-762` overclaims — "It is a normal statement (uses explicit
  return)" — when an inline body is really a restricted subset.
  Confirmed matrix of what an inline body accepts:
  | statement | inline body |
  |---|---|
  | `return` / bare `return` | OK |
  | `pass` | OK |
  | `todo "x"` | OK |
  | `throw "boom"` | OK |
  | bare expression (`io.print("hi")`) | OK |
  | assignment (`z = 9`) | OK |
  | **`discard g()`** | **`expected an expression`** |
  | **`assert 1 == 1`** | **`expected an expression`** |
  | `var a = 1` / `if …` / `break` | `expected an expression` (genuinely need a block — fine) |
- **Repro** — the warning and the trap, in one program:
  ```
  var io = import("io")
  var g = Function(): return 1
  var f = Function(): g()
  f()
  io.print("done")
  ```
  Real output:
  ```
  p.ki:3:22: warning: result of expression is unused; prefix with 'discard' to ignore it intentionally
  done
  ```
  Now follow the advice:
  ```
  var io = import("io")
  var g = Function(): return 1
  var f = Function(): discard g()
  f()
  io.print("ok")
  ```
  Real output:
  ```
  /tmp/a01/p.ki:3:21: error: expected an expression
  ```
  It also fires from a call-argument lambda, the most common inline shape:
  ```
  xs.sort(key = Function(x): g())
  ```
  → `p.ki:4:29: warning: result of expression is unused; prefix with 'discard' …`
- **Impact**: anyone writing a side-effecting inline lambda. They get a warning, follow its explicit
  instruction, and get a parse error that explains nothing. The only workarounds are to convert to a
  block body or to leave the warning on (or silence *all* warnings with `-w`).
- **Proposed fix**: add a `case TokenType::KwDiscard:` to `parseInlineStatement` mirroring
  `parseStatement`'s (build a `DiscardStmt`, `node->expr = parseExpr()`, no statement terminator —
  `parseExpr` naturally stops at the enclosing `,`/`)`/`]`). `assert` is the same one-liner but is more
  debatable inline: `assert cond, "msg"`'s comma would read ambiguously inside a call-argument list, so
  either omit it or accept only the message-less form. Breaks no documented contract — it only *adds*
  accepted syntax.
  While there: `parseInlineStatement` duplicates `parseStatement`'s `KwPass`/`KwTodo`/`KwThrow` arms
  verbatim (e.g. `if (at(TokenType::String)) node->message = advance().text;` appears at both
  `parser.hpp:94` and `parser.hpp:791`) — which is exactly why `discard` was added to one and not the
  other. A shared `parseSimpleStatementCore()` that both call, with the terminator handling left to the
  caller, would single-source it and stop the two lists drifting again (DRY).
- **Proposed test**: `tools/tests/scripts/` (language script) — `var f = Function(): discard g()` then
  `f()` runs with **no** warning; plus a `tools/tests/errors/` case removed/updated if any pins the
  current failure. Also a C++ case in `test_functions.cpp` asserting the inline `discard` body parses.
  Must be verified to FAIL on the current build (it does — see repro).
