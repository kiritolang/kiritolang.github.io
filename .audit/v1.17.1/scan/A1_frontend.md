# A1 — Compile Frontend Audit (lexer / parser / ast / resolver / analyzer / locals / compiler / bytecode)

Round: v1.17.1 · Auditor scope: the compile frontend, adversarially probed on preserved binaries
(`build-bin/ki-asan` with `ASAN_OPTIONS=detect_leaks=0`, `ulimit -s 262144` for deep-recursion tests;
`build-bin/ki-release` for behavior). No source or test files were edited. Every finding below was
reproduced on a real binary.

Verdict: **the frontend is sound.** No crash, no asan/UBSan trigger, no hang, and no wrong parse/eval
was found across the full adversarial battery. Every malformed program produced a clean `KiritoError`
with a location. The one issue found is a **documentation contradiction** (a doc example uses syntax the
language rejects), not a code defect.

---

## Findings

### F1 — LOW (doc-vs-reality). `docs/pages/09-types.md:54` — underscore digit separators in a literal example, but they are unsupported

- **File:line:** `docs/pages/09-types.md:54`
- **Severity:** LOW (documentation only; no runtime impact)
- **CONFIRMED.**
- **Claim:** the Integer section illustrates 64-bit literal wrapping with
  `` `0x1_0000_0000_0000_0000` → `0` `` — a hex literal containing `_` digit separators.
- **Reality:** the lexer has **no** underscore handling in `Lexer::number()` (lexer.hpp:197-248): a `_`
  is not a base/decimal digit, so tokenization stops at it and the trailing `_0000...` lexes as a
  separate identifier — a parse error. The **same doc section**, line 89-90, explicitly states digit
  separators (`1_000`) "aren't supported", so line 54 contradicts it.
- **Reproducer:**
  ```
  var io=import("io")
  io.print(0x1_0000_0000_0000_0000)
  ```
  Actual: `error: expected ')' to close the call` (exit 1). Expected per doc: prints `0`.
  (`1_000` reproduces identically.)
- **Root cause:** the example was written as if separators existed; they never did. The wrapping point
  it teaches is real (verified: `0xFFFFFFFFFFFFFFFF → -1`, `9223372036854775808 → INT64_MIN`,
  `99999999999999999999999999999` wraps without throwing), only the spelling in the example is invalid.
- **Fix (not applied — report-only):** rewrite the example without underscores, e.g.
  `0x10000000000000000 → 0`.

No HIGH/MED findings.

---

## Verified CORRECT (probed, found sound)

### Lexer (lexer.hpp)
- **Integer bases & overflow:** decimal/`0x`/`0b`/`0o` (case-insensitive). Over-wide literals wrap
  two's-complement in u64 space and never throw (`intLiteral`, parser.hpp:1124): `0xFFFFFFFFFFFFFFFF`→`-1`,
  `9223372036854775808`→`INT64_MIN`, 29-digit decimal wraps cleanly, 500 000-digit decimal handled.
- **Empty base body rejected:** `0x`/`0o`/`0b` with no digits → clean `invalid numeric literal`.
- **Float edges:** subnormal `5e-324` yields the value (parseDouble path, not `std::stod`), `1e400`→`inf`
  (no throw), `1.e5`→`100000.0`. Doc rule enforced: `.5` and `1.` both rejected cleanly.
- **`.` vs `...` vs member vs float:** `(1).compare(1.0)` lexes `1`+`.compare`; `...` is its own token.
- **Strings/f-strings:** nested f-strings (`f"{f'{x}'}"`), quote-aware brace matching over embedded
  string literals (`f"{d['a']}"`), colon spec-splitting that ignores `:` inside slices/dicts/strings
  (`f"{s[1:3]}"`, `f"{'a:b'}"`), format specs (`f"{255:x}"`), literal `{{`/`}}`, empty f-string,
  triple-quoted multiline f-strings. Unmatched `{`, lone `}`, empty `{}`, and a newline inside a
  single-line f-string all error cleanly.
- **Escapes unified (common.hpp decoder) across plain & f-strings:** `\q`→`invalid escape`, `\x4`/`\x`→
  `invalid \x escape`, `\x41`→`A`; identical in plain and f-strings. Raw strings keep `\n` verbatim;
  `r"\"` is (correctly) unterminated. Trailing backslash → `unterminated string`.
- **Unterminated tokens:** unterminated single-/triple-quoted strings/f-strings error with a location.
- **Bytes/encoding:** embedded NUL is a valid string char (`len("a\0b")==3`, not truncated); raw control
  byte / non-ASCII lead byte rejected with a printable `\xHH` diagnostic (no message corruption);
  UTF-8 BOM stripped; CRLF and lone-CR normalized to LF; UTF-8 inside string literals passes through.
- **Indentation:** tab/space ambiguity and inconsistent dedent both throw
  `inconsistent use of tabs and spaces` / `inconsistent dedent`. `wide`/`narrow` use int64 (no overflow
  on pathological leading whitespace).
- **Operators:** bare `!` → helpful `did you mean '!=' or 'not'?`.
- **Huge tokens:** 500 KB identifier, 500 K-digit number, 1 MB string literal all handled without crash.

### Parser (parser.hpp)
- **Precedence/associativity:** `-2**2 == -4`, `2**3**2 == 512` (right-assoc `**`), `2+3*4 == 14`.
- **Non-chaining comparisons:** `1 < 2 < 3` rejected with the "connect with and/or" diagnostic.
- **Deep-nesting anti-overflow — the key hunt.** `DepthGuard` + `ChainDepth` share one `exprDepth_`
  budget (`kMaxParseDepth` = 2000 release / 250 sanitizer). Under `ulimit -s 262144` + asan, every one of:
  100 K nested `()`, `[]`, `{}`, `{k:...}`; 200 K `+` chain; 100 K unary `-`; 50 K `not`; 100 K index /
  member chains; 50 K `**`; 50 K ternary; deep call-arg / slice nests; deep indent pyramid — throws
  `expression nested too deeply` **before** any overflow. Confirmed **no later pass can be reached with a
  deeper tree than the parser admits**: at the default 8 MB stack (no `ulimit`), a 1990-deep `if` nest
  runs to completion, and the parser gate (~1997) is tighter than the resolver (2800), analyzer (2500),
  and compiler (3000) guards, so those never trip in practice. Off-by-one at the boundary is clean
  (1990 OK, ≥1998 rejected). AST teardown of the rejected trees is exception-safe (RAII `ChainDepth`).
- **try/catch/finally:** `try` with no `catch`/`finally` rejected; bad catch-type expr → clean error.
- **switch:** empty body rejected; soft keywords `case`/`default` stay usable as names.
- **Comma packing / targets:** bare tuples, `a, = x` 1-element unpack, `*a,*b` two-star rejection,
  `*a,1` star-in-value rejection, trailing commas, positional-after-keyword (compiled to a catchable
  runtime throw), non-default-after-default, duplicate-param hard error.
- **Subscript/slice:** empty `a[]` rejected; single-axis slice, multi-axis subscript, slice assignment
  (`a[1:3]=[9]`), chained index assign (`a[0][1]=9`) all correct.
- **Inline vs block function bodies:** inline-fn comma-pack ambiguity rejected; block-fn dedent
  self-termination (`blockJustClosed_`) correctly stops next-line `(`/`[`/`.`/`,`/`if`/operator gluing.
- **Member names:** keyword-as-attribute allowed (`set.discard`), string/number literal after `.`
  rejected (`x."foo"`), `1.` → `expected a member name`.
- **Malformed/empty input:** empty file, comment-only, whitespace-only → exit 0; `var =`, `var x`
  (no init), dangling `elif`/`else`, unclosed `(`/`[` at EOF, `if 1:` with no block, chained assignment
  `a=b=2`, `for x [1]` (no `in`), assignment with no value — all clean single-line errors, no crash.

### Resolver / analyzer / locals (resolver.hpp, analyzer.hpp, locals.hpp)
- **Name resolution by membership:** forward refs, recursion, mutual recursion (`isEven`/`isOdd`) all
  resolve; genuinely-undefined names caught at compile time (`name '…' is not defined`); read-before-
  assignment of a real top-level slot surfaces as a catchable runtime error (not a crash).
- **Closure capture / slot layout (the desync-risk area):** loop-var capture, param default referencing
  an earlier param (`Function(n, size=n)`), a default capturing an enclosing-function local, param
  shadowed by a body `var` (warns + rebinds), captured-and-reassigned outer local (`counter/inc`
  yields 1 2 3), class-method sibling reference, and a 3-deep capture chain (`x+y`) — **all produce
  correct values under asan** (no wrong-slot read, no assertion abort). The resolver/compiler/runtime
  agree on env-slot layout via the shared `collectBlockDeclsOrdered`/`capturedLocals` in locals.hpp.
- **Analyzer lints** (unused var, shadowing footgun, unreachable code, self-assignment) fire as designed
  and are non-fatal. Statement recursion is unguarded but bounded by the tighter parser gate (verified
  safe at 8 MB stack).

### Compiler / bytecode (compiler.hpp, bytecode.hpp)
- **Constant folding correctness (`foldConstValue`):** only pure Literal/Unary/Binary/Logical fold, via
  the VM's real operators, so a switch label keys identically to the runtime. Verified: `case 3+4`
  matches `7` and de-dups against `case 7`; `case "a"+"b"` matches `"ab"`; `case 1.0` ≠ `case 1`
  (type-exact); `case 1/0` throws **at compile time** at the label span; a non-constant / non-scalar /
  duplicate label is a compile error. Integer-overflow folding (`9223372036854775807+1`) is well-defined
  wrapping and matches runtime — correctly folded, not a bug.
- **Assignment targets:** name/index/member/slice/tuple(+star) all emit correctly; invalid targets
  (`1=2`, `f()=2`) throw `invalid assignment target`.
- **Control flow crossing cleanups (A04-1 class):** `return` crossing a `finally` inside a loop
  preserves the return value and runs the finally at a clean operand height; `break` inside a `finally`
  correctly pops the for-cursor; `continue` crossing a finally; `return`/`break` crossing a `with` runs
  `_exit_`; nested try-finally return ordering (`inner`/`outer`/value) — all correct under asan.
- **Depth guard** (`DepthScope`, 3000) never reachable behind the tighter parser gate; provides margin
  for deserialized/f-string-sub-parsed ASTs (which themselves re-enter the bounded parser).

---

## Method notes / honesty
- All deep-recursion cases were run under **asan + `ulimit -s 262144`**; behavior cases under
  ki-release. A flat 200 000-statement program (non-nested) compiles and runs (exit 0) — slow under asan
  purely from instrumentation overhead, not a defect.
- The resolver's `depth_ > 2800` early-return and `intLiteral`'s invalid-digit→0 fallthrough are both
  **unreachable** given upstream guarantees (parser depth gate; lexer digit validation) — verified, not
  merely assumed.
- This is a genuine clean pass: the adversarial inputs above are the evidence, and the only actionable
  item is the F1 doc fix.
