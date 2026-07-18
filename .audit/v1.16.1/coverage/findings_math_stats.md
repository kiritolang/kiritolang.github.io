# Coverage findings — numeric stdlib (math, statistics, random, bisect, heapq)

Test: `tests/scripts/cov_math_stats.ki` (+ `.expected`), 325 assertions, exit 0.
All behavior observed by running `./build-release/ki`. Impl:
`src/kirito/stdlib_math.hpp`, `stdlib_random.hpp`, `stdlib_kimodules.hpp` (statistics/bisect/heapq are
Kirito-authored there).

## Bugs / silent errors

None. Every documented domain/hardening case that should throw does throw, with the documented
message substring. No silent NaN/inf returns, no silent overflow wraps, no should-error case that
succeeded.

## Doc-vs-impl deltas

None material. Everything documented matches observed behavior, including the deliberate divergences
the docs already call out:

- `heapq.heapify(items)` returns a NEW list and leaves `items` untouched (docs state this explicitly;
  diverges from CPython's in-place `heapify`). Confirmed.
- `statistics.quantiles` uses the exclusive method and EXTRAPOLATES past the data range on the tail
  cuts — `quantiles([1, 2]) == [0.75, 1.5, 2.25]`. Confirmed exactly.
- `math.trunc` returns a Float (not Integer, unlike `floor`/`ceil`). Confirmed.
- `math.pow` always returns a Float. Confirmed.
- `perm(n)` / `perm(n, None)` return `n!`. Confirmed.

## Observations (non-blocking)

- `heapq.merge(lists)` pushes every element of every sublist into a fresh heap and pops them all —
  a full heapsort (O(N log N)), not a k-way merge exploiting already-sorted inputs. Output correct
  for the documented contract (and even for unsorted sublists). Efficiency note only.
- `random.uniform(a, b)` documented `[a, b)`; exact-upper exclusion is a property of
  `std::uniform_real_distribution` and not deterministically testable, so the test asserts range
  membership over 200 seeded draws only.
- Secure CSPRNG functions covered by length/type/range invariants + error paths only (unpredictable
  by design).
