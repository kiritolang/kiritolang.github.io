# Doc-vs-implementation verification & exhaustive small-test pass ("label X" loop)

**Goal.** Go through the documentation feature by feature, confirm each documented function / class /
method / attribute actually behaves as described, and back every one with small `.ki` tests that cover:
functioning cases (exact expected output), **edge cases** (empty / boundary / degenerate / Unicode /
huge / negative / zero / type-mixed — whatever "edge" means for *that* feature), and **hardening**
(does bad input raise, and with the *right* message? — and, conversely, is there any case that
*should* raise but silently doesn't = a silent bug in Kirito).

**Rules.**
- Do **NOT** modify `src/` (no C++). Only add `tools/tests/scripts/verify_*.ki` (+ `.expected`) — and
  `tools/tests/errors/*.ki` (+ `.experr`) for parse-time errors that can't be caught at runtime.
- If a real bug (or a doc/impl mismatch) is found: record it in [`DISCREPANCIES.md`](DISCREPANCIES.md).
  Only if it is a genuine defect may `src/` be touched — flag it first.
- If the docs are **missing** a real feature, add it to the relevant `docs/pages/*.md`.
- Re-verify everything even if previously tested. Thoroughness over novelty.

**Coverage** is tracked in [`INVENTORY.md`](INVENTORY.md) (every module + its documented surface, with
a done/notes column). When every row is covered, cross-check the docs symbol index one more time; any
gap sends us back to **label X**.

## Canonical test-script shape

Each `verify_<area>.ki` imports what it needs, defines the two helpers below, asserts functioning +
edge cases, asserts hardening via `raises`/`raises_msg`, and prints one final `OK <area>` line (which
is the whole `.expected`). Asserts fail loudly with a message, so a regression is unmistakable.

```kirito
var io = import("io")

# returns True iff calling fn() throws (any error)
var raises = Function(fn) -> Bool:
    try:
        discard fn()
        return False
    catch:
        return True

# returns True iff fn() throws AND the message contains `needle` (hardening: the RIGHT error)
var raises_msg = Function(fn, needle) -> Bool:
    try:
        discard fn()
        return False
    catch String as e:
        return needle in e
```

(`catch String as e` binds the message; a bare `catch` also catches any native `std::exception`.)

## Module assignment (one file per area)

builtins · types (None/Bool/Integer/Float) · types (String/Bytes) · types (List/Set/Dict) · operators+dunders ·
io · path · math · random · tensor · matrix · complex · json · serialize+dump · net · sys · time ·
zlib+gzip · hash · regex · parallel · itertools · functools · collections · statistics · string ·
textwrap · base64 · csv · tabular · xml · heapq · bisect · copy · enum · tee · arg · semver
