# Doc-vs-impl: statistics, string, textwrap (Kirito-authored stdlib_kimodules.hpp)

Verified `/home/user/kiritolang.github.io/build-debug/ki` against docs `docs/pages/10-stdlib.md`
(`## statistics`, `## string`, `## textwrap`) and impl `src/kirito/stdlib_kimodules.hpp`.
Tests: `tools/tests/scripts/verify_{statistics,string,textwrap}.ki` (+ `.expected`).

## Genuine discrepancies / defects
None. Every documented function behaves as documented. No `src/` change warranted.

## PINNED behaviours (current impl — not defects, but non-obvious / worth locking in)

### statistics
- **Bool is NOT treated as 0/1.** The task brief anticipated "Bool treated as 0/1", but Kirito
  arithmetic rejects Bool operands entirely: `0.0 + True` -> `unsupported operand type 'Bool' for
  arithmetic with 'Float'`, and even the builtin `sum([True, False])` -> `sum expects numbers`.
  Consequently:
  - `mean([True, False])` RAISES (adds Bool to a Float accumulator).
  - `median([True, False])` RAISES with `cannot order 'Bool' and 'Bool'` (its `sorted()` fails).
  Only explicit `Integer(True)` converts. PINNED as raising, not silently coerced.
  The docs do not claim Bool support for statistics, so this is not a doc mismatch — just a pin.
- **`multimode([])` returns `[]` and does NOT raise** — asymmetric with `mode([])`, which throws
  `no mode for empty data`. This is deliberate (multimode's count loop simply produces nothing) and
  is now pinned; if it should raise, that is a design call, not a bug.
- **`quantiles` checks the data-size guard before the `n` guard.** `quantiles([1], 0)` reports
  `quantiles requires at least two data points` (size), never reaching the `n must be at least 1`
  check. Order pinned.
- **Large-integer mean does not overflow.** `mean([2^62, 2^62])` stays ~4.6e18 positive because the
  accumulator is a Float and each element is promoted per-add (impl comment confirms this is
  intentional to avoid an int64 wrap that `Float(sum(data))` would suffer).
- Exact values pinned: `variance([2,4,4,4,5,5,7,9]) = 32/7`, `pvariance = 4.0`, `pstdev = 2.0`;
  `quantiles([1,2,3,4,5]) = [1.5, 3.0, 4.5]`; `quantiles(_, 1) = []`.

### string (the MODULE, not the String type)
- Constants pinned to EXACT contents: `ascii_letters` = lower+upper (52 chars),
  `hexdigits = "0123456789abcdefABCDEF"`, `octdigits = "01234567"`,
  `punctuation` = the 32-char ASCII punctuation set, `whitespace = " \t\n\r"` (4 chars).
- `capwords` splits on **single spaces only**: runs of spaces are preserved as empty words
  (`"a  b" -> "A  B"`), and tabs are NOT separators (`"a\tb" -> "A\tb"` — one word). Matches docs.
- `similarity("", "") == 1.0`; `similarity(a, [])` returns `[]` (List form).
- `similarity/closest/fuzzymatch` dispatch on `query.levenshtein(...)`, so a **non-String query
  raises** (no `.levenshtein` method) — pinned as hardening.
- `closest([])` -> `None`; `fuzzymatch` returns `[candidate, score]` pairs sorted by score
  descending, default cutoff `0.6`.

### textwrap
- `wrap("", w)` -> `[]` (empty text yields no lines).
- `wrap` splits on **spaces only** — an embedded `\n` is glued into a "word" (`"a\nb c"` with wide
  width -> `["a\nb c"]`); no newline-aware breaking. Pinned.
- `wrap(text, 0)` and negative widths put **each word on its own line** (an over-long word is never
  truncated — it always occupies a full line regardless of width). Default width is 70.
- `fill = "\n".join(wrap(...))`.
- `indent` prefixes only **non-empty** lines (empty lines pass through unprefixed);
  `indent("", prefix) == ""`. `indent` requires the prefix arg (no default) and a non-String prefix
  raises (`String + Integer` unsupported).
- `dedent` ignores blank / whitespace-only lines when computing the common indent and preserves
  them; treats tabs as leading whitespace by character count (no tab-expansion).

## Test coverage
- `verify_statistics.ki` — 49 asserts: mean/median/mode/multimode/variance/pvariance/stdev/pstdev/
  quantiles functioning + edge (single/two/even/odd/all-equal/large-int) + hardening (empty raises,
  <2 for sample variance, n<1 for quantiles) + silent probes (multimode([]), Bool-not-numeric).
- `verify_string.ki` — 48 asserts: constants (exact + lengths), capwords, similarity (String + List
  forms), closest (typo/ties/empty), fuzzymatch (cutoff/sort) + hardening (non-String query raises).
- `verify_textwrap.ki` — 37 asserts: wrap/fill/indent/dedent functioning + edge (width<=0, empty,
  embedded newline, blank lines, tabs, over-long word) + hardening (non-String raises, missing arg).

All three produce exactly their `.expected` (`OK <area>\n`).
