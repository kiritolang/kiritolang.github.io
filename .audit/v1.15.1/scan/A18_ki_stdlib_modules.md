# A18 — Kirito-authored, frozen-source stdlib modules (v1.15.1)

**Scope:** `src/kirito/stdlib_kimodules.hpp` — the modules written IN Kirito and frozen as source:
`itertools, functools, collections, statistics, string, textwrap, base64, csv, tabular, xml, heapq,
bisect, copy, enum, tee, arg, semver`.

**Method:** probe the real binary (`./build-debug/ki`), paste real output. `--gc-threshold 1` soak on a
sample. Also audit test coverage (`tools/tests/scripts/*`, `tools/tests/unit/*`).

**Pinned false positives (NOT to be "fixed")** — from `.audit/README.md`:
- `statistics.quantiles` CLAMPS the extremes (no CPython extrapolation).
- `base64.decode` REJECTS embedded whitespace/newlines.
- `semver.parse` ACCEPTS leading-zero numerics.
- `tabular.DataFrame` silently truncates a too-long ragged row / throws on too-short / does not
  validate a user `index=` length.

**v1.15 context:** A16-1 (`DataFrame(rows, columns=N)` reports when N doesn't fit), A16-2
(`heapq.heapify` is Floyd bottom-up O(n); `_siftdown(heap, pos)` signature changed, shared with
`heapreplace`).

---

## Findings

### A18-1: `deque.pop()`/`popleft()` on an empty deque leaks the internal List  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/stdlib_kimodules.hpp:313-316` (`class deque` `pop`/`popleft`)
- What: both delegate straight to `self._items.pop()`, so an empty deque reports the *implementation's*
  type, not the library's. The class is otherwise careful to present itself as a `deque` (`_str_` prints
  `deque([...])`). The brief calls this class out explicitly ("a module error message ... leaks
  internals").
- Repro:
```
var co = import("collections")
co.deque().pop()        # => pop from empty List
co.deque().popleft()    # => pop from empty List
```
  Real output:
```
deque pop empty !! pop from empty List
deque popleft empty !! pop from empty List
```
- Impact: a user who never touched a List gets told a List is empty. Minor, but it is the only
  user-visible seam where `deque` admits it is a List.
- Proposed fix: guard both — `if len(self._items) == 0: throw "pop from empty deque"`. No contract risk
  (nothing pins the current text; grep of `tools/tests` finds no assertion on it — see coverage table).
- Proposed test: `tools/tests/scripts/r7_kimods_a.ki` (or wherever deque lives) — assert the message
  says `deque`, not `List`.

_(appended as confirmed)_
