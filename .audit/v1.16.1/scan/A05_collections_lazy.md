# A05 — collections + lazy iterators + generators (v1.16.1)

Scope: collections.hpp (DictVal/SetVal/ListVal), runtime.hpp (lazy seq: Range/Map/Filter/Zip/Enumerate,
SourceCursor, streamIterate/drainLazy/rootedIterate, generator protocol, collection type-methods),
object.hpp (LazyIterator seam), bytecode_vm.hpp (IterCursor GetIter/ForIter).

Overall: this surface is genuinely solid and asan-soaked. The Probe/ProbeScope reentrancy guards,
snapshot-based iteration, GC rooting in every lazy combinator, and PEP-479 attribution all hold up under
adversarial reading and live testing. One real (latent) UB found; the rest are low-severity / semantic
observations and test-coverage gaps.

## Findings

### F05-1  [Med]  `range.contains` (`x in range(...)`) has signed-overflow UB for wide ranges
- runtime.hpp:3159-3166 (`RangeVal::contains`) — `return (x - start_) % step_ == 0;`. The comment
  claims "x,start_ both in-range → subtraction fits", but that is false when the range spans more than
  half the int64 domain. `x - start_` is a signed int64 subtraction; for `start_ == INT64_MIN` and any
  in-range `x >= 0` (or symmetrically for the negative-step branch), `x - start_` overflows int64 → UB.
  This is reachable within the 32M element cap because a large `step` keeps the element *count* small
  while the *span* is the full domain: `range(INT64_MIN, INT64_MAX, 6000000000000)` has count ≈ 3.07M
  (< kMaxRangeCount) yet `0 - INT64_MIN` overflows.
- Trigger/repro:
  ```
  var r = range(-9223372036854775808, 9223372036854775807, 6000000000000)   # count ~3.07M, constructs OK
  var b = 0 in r          # evaluates (x - start_) = 0 - INT64_MIN  -> signed overflow UB
  ```
  Confirmed: the range constructs (len == 3074458) and `0 in r` runs; the subtraction is UB. Debug
  build has no UBSan so it does not abort, but the asan/tsan presets DO enable `-fsanitize=undefined`
  — a test exercising this would trip it.
- Fix idea: do the offset math in unsigned, matching `count()`/`at()`:
  `uint64_t off = static_cast<uint64_t>(x) - static_cast<uint64_t>(start_);` then test
  `off % (step_>0 ? (uint64_t)step_ : (0u - (uint64_t)step_)) == 0`. (The in-range guard already
  guarantees `off` is a true multiple offset within the span, and the span fits in uint64.)
- Test to add: `assert not (0 in range(-9223372036854775808, 9223372036854775807, 6000000000000))`
  plus a positive hit and the negative-step mirror — run under the asan preset (UBSan) so the overflow
  is actually caught, not just the boolean result.
- Verified-real: yes — constructed the range live and confirmed the code path reaches `x - start_`
  with `start_ == INT64_MIN`. UB is by inspection of the two's-complement subtraction.

### F05-2  [Low]  Dict/Set entry index positions are `int32_t` — silent corruption past 2^31 entries
- collections.hpp:161-164, 210-231, 447-449 — `index` stores entry positions as `int32_t`; `set`/`add`
  do `index[slot] = static_cast<int32_t>(np)` where `np = entries.size()`. Once `entries.size()`
  exceeds `INT32_MAX` (~2.1e9), the cast wraps to a negative value that collides with `kEmpty` (-1) /
  `kDeleted` (-2), silently corrupting probe results. Practically unreachable (2.1e9 entries ≈ 64+ GB
  for a Dict), so severity Low — but it is an undocumented hard ceiling with no guard (unlike the
  range/repeat caps, which throw a clean error).
- Fix idea: either widen to `int64_t` (doubles the index memory) or add a defensive
  `if (entries.size() > INT32_MAX) throw` in `set`/`add`, mirroring the "too large" guards elsewhere.
- Test to add: not feasible at that scale; a comment/guard is the realistic mitigation.
- Verified-real: yes by inspection (the cast is unguarded); not runtime-reproducible at feasible sizes.

### F05-3  [Low]  `map`/`filter`/`zip`/`enumerate` are RE-iterable, unlike Python's one-shot iterators
- runtime.hpp:3223-3335 — each `lazyIterate()` builds a *fresh* SourceCursor, so iterating the same
  `map`/`filter`/`zip`/`enumerate` object twice restarts it (when its source is restartable) rather than
  yielding empty on the second pass. `List(m); List(m)` returns `[2,4,6]` both times (confirmed).
- This is arguably friendlier than Python, but it is a documented-behavior divergence and a footgun if a
  source is a *one-shot* stream (a file / a user `_iter_`-returns-self generator): the second pass
  silently re-drives `_iter_`/re-opens semantics and may yield different or empty results depending on
  the source. No correctness bug in the combinator itself.
- Test to add: pin the intended contract explicitly — e.g. that `map` over a restartable source (list/
  range) restarts, and document that over a self-exhausting generator it does not. (Currently only the
  fresh-`_iter_` re-iterable case is pinned, in spec_generators.ki.)
- Verified-real: yes (behavior confirmed live). Filed as a semantic note, not a defect.

### F05-4  [Low]  `filter(None, xs)` throws instead of Python's truthiness-filter
- runtime.hpp:3770-3773 + FilterLazyIterator (3235-3241) — `filter` unconditionally calls the predicate;
  passing `None` yields a lazily-surfaced "type 'None' is not callable" on first pull, not a Python-style
  identity filter that keeps truthy elements. Minor semantic gap; the error is at least clean and
  catchable. Worth either documenting or supporting `None` (fall back to `.truthy()`).
- Verified-real: yes (confirmed live).

## Coverage notes

Existing coverage is strong. Dedicated golden scripts: `spec_lazy.ki` (range back-compat, map/filter/
zip/enumerate laziness + types + non-indexability, streaming reductions, short-circuit over infinite
generators), `spec_generators.ki` (`_iter_`/`_next_`, self- vs separate-iterator, re-iterability,
infinite+break, catchable/isinstance StopIteration, strict PEP-479 deep-leak, old-style `_iter_`-returns-
List), `spec_iter_mutation.ki` (snapshot semantics for List/Dict/Set/reversed/enumerate/zip under
append/remove/clear/index-assign), plus `collections.ki`, `spec_collections.ki`, `r4_collections.ki`,
`test_collections.cpp`, `test_gc_generational.cpp`, `test_gc_stress.cpp`. Confirmed live: PEP-479 deep
attribution, infinite generator + break, `any(map(pred, infinite))` short-circuit, map re-iteration,
nested map/filter/zip/range, range==List both directions, enumerate int64 wrap (safe via `wadd`).

UNTESTED / gaps worth adding:

- **range.contains overflow (F05-1)** — no test exercises a wide range whose `contains` overflows; add one
  under UBSan.
- **range negative-step membership with wide span** — `x in range(INT64_MAX, INT64_MIN, -big)` (the
  negative branch of the same overflow) is untested.
- **`popitem` LIFO order + NaN key drain** — `DictVal::popArbitrary`/`SetVal::popArbitrary` LIFO order is
  not directly asserted; the "drain a Dict/Set that contains a write-only NaN key via popitem/pop" loop
  (the documented reason popArbitrary takes by position) has no golden test. Add:
  `d = {float("nan"): 1}; while len(d) > 0: d.popitem()` must terminate.
- **NaN write-only key after rehash** — inserting enough distinct keys to force a `reindex`, with a NaN
  key present, then confirming the NaN slot is still position-addressable by popitem (its `slotOfPos`
  path) is untested.
- **Tombstone compaction trigger** — churning a Dict/Set with interleaved add/remove of >8 tombstones so
  `reindex(compact=true)` fires (collections.hpp:335/467), then verifying contents + `len` + card
  validity, is not directly covered.
- **`keysEqual` cross-type Integer/BigInt/Float in a live Dict/Set** — the symmetric-retry (stored native
  Integer vs probe BigInt) is documented (r7) but a focused test that `big(2)` and `2` collapse to one
  Dict/Set bucket *in both insertion orders* is worth pinning here.
- **ProbeScope: user `_eq_`/`_hash_` that mutates the container mid-probe** — the "changed size during a
  key/value comparison" rejection is only lightly exercised; add a class whose `_eq_` calls `d.clear()`/
  `d[k]=v` while it is a Dict/Set key and assert the clean catchable error (no UAF, asan-clean).
- **Set intersection/difference/symmetricdifference/issubset/... over a LAZY `other`** — these use
  `.iterate(vm)` (eager); passing `map(...)`/a generator as `other` is untested (works via `MapVal::
  iterate`/`InstanceValue::iterate`, but unpinned).
- **`zip` shortest-stop where a MIDDLE column (not the last) is shortest**, and zip of 3+ sources — only
  2-source truncation is tested.
- **enumerate/map/filter over a source that mutates during a STREAMING for-loop** (lazy source buffered
  via SourceCursor) — iter_mutation covers eager snapshot; the lazy-cursor buffering path isn't asserted.
- **`List(infinite_generator)` / `sorted(infinite)`** — no test pins that eager consumers of a truly
  infinite generator run away (expected, like Python) vs. any guard; worth a documented note.
- **int32 index ceiling (F05-2)** — untestable at scale; guard/comment only.
