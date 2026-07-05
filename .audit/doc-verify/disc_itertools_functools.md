# itertools / functools — doc-vs-impl verification

Tests: `tools/tests/scripts/verify_itertools.ki` (82 asserts) + `verify_functools.ki` (23 asserts).
Impl: `src/kirito/stdlib_kimodules.hpp` (`itertools` src @L14-255, `functools` src @L258-295).
Docs: `docs/pages/10-stdlib.md` (`## itertools` @L412, `## functools` @L282).

Run against `build-debug/ki`. Both print their `OK` line byte-exact. **No genuine defects found** —
every documented function behaves as described. Behaviours worth PINNING (documented-but-subtle, or
undocumented edge behaviour that the tests now lock in) below.

## Pinned behaviours (not bugs — regression guards)

# DISCREPANCY: none are defects. The following are exact-behaviour PINS.

- `itertools.count(start, step, stop)` — param order is `start, step, stop` (step BEFORE stop, unlike
  `range(start, stop, step)`). Documented, and pinned. `count(0,2,10) == [0,2,4,6,8]`.
- `count` requires a `stop` bound (`stop == None` throws `itertools.count needs a stop bound (no lazy
  generators)`) and `step != 0` (throws `itertools.count step must not be zero`). Only these two
  guards; a float step is fine (`count(0, 0.5, 2) == [0, 0.5, 1.0, 1.5]`).
- `repeat` / `cycle` — `times` is a REQUIRED positional (no default). Omitting it throws
  `function missing required argument 'times'` (arity error, not a bespoke message). A NEGATIVE
  `times` yields `[]` (it drives `range(times)`), it does not throw.
- `islice(iterable, start, stop, step=1)` — `step == 0` does NOT have its own guard: it reaches
  `(i - start) % step` only once `i >= start`, so `islice([1,2,3], 0, 3, 0)` throws `integer modulo by
  zero`, but `islice([1,2,3], 0, 0, 0)` (empty window, never reaches the modulo) returns `[]` silently.
  Both pinned.
- `accumulate([iterable][, func])` — the LEADING element is taken verbatim (func applied from the 2nd
  on). A leading `None` is treated as a real value, so `accumulate([None, 1, 2])` raises (`None + 1`
  unsupported) rather than treating None as "unset". Matches the impl's own comment; NOT stated in docs.
- `product([])` → `[[]]`; a pool that is empty collapses the whole product to `[]`
  (`product([[1,2],[]]) == []`).
- `permutations([])` → `[[]]`; `permutations(items, 0)` → `[[]]`; `r > n` or `r < 0` → `[]`.
- `combinations(items, 0)` → `[[]]`; `r > n` or `r < 0` → `[]`.
- `groupby` groups only CONSECUTIVE runs — a key that recurs after a different key forms a SEPARATE
  group: `groupby([1,1,2,2,3,1]) == [[1,[1,1]],[2,[2,2]],[3,[3]],[1,[1]]]`.
- Resource guards (cap `_MAXCOMBINATIONS = 10_000_000`) throw a clean, catchable error from a
  PROJECTED-size check before materialising, so they are fast (~0.05s total for all three):
  - `product` → `product: result too large (> 10000000 combinations)` (checked per pool, up front).
  - `permutations` → `permutations: result too large (> 10000000 permutations)` (multiplicative count).
  - `combinations` → `combinations: result too large (> 10000000 combinations)` (binomial recurrence).

- `functools.reduce(func, iterable[, initial])` — `initial` is an IDENTITY sentinel (`_reduce_unset`),
  so an explicit `None` seed is a REAL seed (`reduce(f, [], None) == None`), distinct from omitting it.
  Empty iterable with NO initial throws `reduce of empty sequence with no initial value` (doc-exact).
- `functools.partial(func, bound)` — `func` receives ONE List (bound ++ rest); `bound` is SNAPSHOTTED
  at creation (`.copy()`), so mutating the caller's list afterwards does not leak in, and the wrapper
  does not accumulate across calls. Matches docs.
- `functools.cache(func)` — memoizes a single-arg fn in a Dict. An UNHASHABLE arg (List/Dict) throws
  `unhashable type '<name>'` (the `x not in store` membership test), matching the doc note. Undocumented
  guards: none beyond hashability.

## Doc-gap note (minor, not filed as a defect)

The docs do not mention the resource-guard error messages, the required-bound errors, or that
`accumulate` folds a leading `None` as a value. These are hardening/edge behaviours; the tests pin
them. No `docs/` edit made — the docs are accurate about the happy path.
