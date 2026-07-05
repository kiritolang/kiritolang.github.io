# v1.13 audit — triaged findings

Deduped, ranked roll-up of the per-agent findings in `agents/`. Status: `fixed`, `test-added`,
`todo`, `wontfix` (with reason). The scan is ongoing; this file grows as waves land.

## Confirmed bugs — CORE wave (A01–A10)

### HIGH (memory-safety / correctness) — all FIXED + TESTED + VALIDATED (debug 727/727 + asan/ubsan clean)

| ID | Bug | Fix | Status |
|----|-----|-----|--------|
| A05-1 | `Op::GetIter` eager branch: `RootScope` scoped to the `else` block is destroyed before `alloc(cursor)` (which GCs before insert) → fresh iterator items (String chars, Bytes ints, `_iter_` results) swept → UAF. `for c in "abc"` under GC pressure. | Hoist the `RootScope` across the `alloc`. | fixed + test |
| A03-1 | Capture analysis: a nested function's parameter defaults were scanned at the incoming `inNested` (false when nested directly in F), but a default is evaluated through the nested fn's own closure — an F-local ref there is a real capture. Wrongly frame-slotted → silent wrong value (105 vs 15) or spurious NameError. | Scan nested defaults at `inNested=true`, growing `inner` over earlier params. | fixed + test |
| A09-1 | List `remove`/`index`/`count`/`in` searched over a live `elems` reference (or range-for iterators) while `kiEquals` could run a user `_eq_` that reallocs the list → UAF (`Evil() in xs`). | Search a GC-rooted snapshot; erase from the live list bounds-checked. | fixed + test |
| A04-1 | `return EXPR` crossing a `finally` that does `break`/`continue`: the value stayed on the operand stack, so the break's cursor-`Pop` removed the return value not the loop cursor → orphaned cursor → enclosing loop corruption (observed: infinite hang). | Park the return value in a hidden local across the crossed cleanups (mirrors `emitFinallyExc`). | fixed + test |

### MEDIUM (correctness / contract) — todo

| ID | Bug | Proposed fix |
|----|-----|--------------|
| A01-1/A01-2 | f-string escape decoder diverges from the plain-string one: `\xHH` ≥0x80 emits a raw byte (breaks the code-point invariant, `f"\xff" != "\xff"`); an unknown escape is silently dropped (`f"\q"`→`"q"`) instead of the documented lex error. Root: **A01-3 DRY** — escape decoding duplicated between `lexer.hpp` and `parser.hpp::parseFString`. | Extract one shared escape decoder; fixes A01-1/A01-2. |
| A02-1 | `var f = Function(): return a, b` silently misparses: inline body's `return` reads with `parseExpr` (stops at comma), the enclosing `parseValueSeq` absorbs `, b` → `f = [<fn>, b]`. Root: **A02-11 DRY** — `parseInlineStatement` duplicates `parseStatement`. | Unify inline/block statement parsing (or make the inline return pack). |
| A04-2 | Duplicate **negative** switch case values (`case -1: … case -1:`) silently accepted — `-1` is `UnaryExpr`, not `LiteralExpr`, so the duplicate detector misses it. (A04-3: a single `case -1` also silently downgrades the whole switch O(1)→O(n).) Root touches **A04-12 DRY** — switch-key logic in two headers must agree. | Fold constant unary-neg literals before duplicate detection / key building. |
| A06-1 | `!=` asymmetry: with a one-sided user `_ne_` the reflected path differs from `==`'s. | (verify) mirror the `_eq_` reflection in `_ne_`. |
| A07-1 | `_iter_` returning `self` → unbounded native recursion → SIGSEGV (uncatchable); `iterate` lacks the cycle guard `str` has. | Add a depth/cycle guard to `InstanceValue::iterate`. |
| A07-5 | `_str_` return value not type-checked (unlike `_bool_`/`_hash_`/`_len_`). | Validate `_str_` returns a String. |
| A09-2 | Sorting / min / max on a NaN-containing List uses `kiLessThan`, not a strict-weak-ordering with NaN → `std::stable_sort` UB. | Define a total order for NaN (or reject / partition), as the tensor/numeric side already reasons about. |
| A10-1 | `"İ".lower()` / Unicode case-fold edge (per A10). | (verify against the documented ASCII+Latin-1+Latin-Ext-A scope.) |

### DRY / single-source-of-truth (root causes above)

- **A01-3** escape decode duplicated (lexer vs parser) → causes A01-1/A01-2.
- **A02-11** `parseInlineStatement` vs `parseStatement` → causes A02-1.
- **A04-12** switch scalar→key logic triplicated (`literalSwitchKey`/`scalarSwitchKey`/`scalarConstKey`).
- **A07-14** positional+named arg binding triplicated (`bindArgs`/`KiFunction::callFull`/`InstanceValue::callKw`).
- **A09-6** Set `remove`/`discard` duplicated; `ValueKind::Array` is phantom dead code (~8 dead branches).
- **A03-15** resolver/analyzer/locals AST walks triplicated (structural).

### Notable LOW / weak-spots (see per-agent .md for the full list + coverage gaps)

A01-5 (NUL in a lex error message), A01-6 (locale-dependent `isalpha` on high bytes), A02-2
(f-string sub-expr error location), A05-2 (`excSpan_` clobbered by nested try in a finally), A07-2
(`findMethod` recurses — deep inheritance stack overflow), A07-4 (equals/hash asymmetry with no VM),
A09-3 (`[] in dict` throws vs `[] in set` returns False), A09-4 (Dict/Set buckets never shrink).

Coverage gaps are logged comprehensively per agent (the bulk of each `.md`); they feed the
test-writing phase (C++ then `.ki`).

## Remaining scan (todo): A11–A35 (stdlib native, ki-modules, builtins, cross-cutting, meta).
