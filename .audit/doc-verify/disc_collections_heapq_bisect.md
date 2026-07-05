# Doc-vs-impl audit: `collections`, `heapq`, `bisect`

Source of truth: `src/kirito/stdlib_kimodules.hpp` (Kirito-authored, frozen-source modules).
Docs: `docs/pages/10-stdlib.md`. Tests: `tools/tests/scripts/verify_{collections,heapq,bisect}.ki`
(+ `.expected`). Verified against `build-debug/ki`.

**Verdict: docs and implementation agree. No defects found. No `src/` changes needed.**
The notes below PIN the current behaviour so a future regression is caught.

---

## collections

Documented surface = actual surface. Three classes, exactly the documented members.

### deque — PINNED
- Members: `append`, `appendleft`, `pop` (rightmost), `popleft` (leftmost), `_len_`, `_getitem_`
  (delegates to the backing List, so **negative indices work**: `dq[-1]`), `_str_`, `_iter_`.
- `_str_` form is exactly `deque([...])` using the List repr (`deque([1, 2, 3])`, `deque([])`).
- Ctor `deque([iterable])`; `deque()` and `deque(None)` both build an empty deque.
- **No `rotate`, no `extend`, no `maxlen`** — confirmed absent (docs correctly omit them). This is a
  reduced surface vs Python's `collections.deque`; that is by design, not a bug.
- Hardening (all raise, catchable as String):
  - `pop`/`popleft` on empty → `"pop from empty List"` (bubbles up from the backing `List.pop`).
  - `dq[i]` out of range → `"index out of range"`.
- No `_setitem_`/`_contains_` (item assignment / `in` are not part of the documented surface).

### Counter — PINNED
- Members: `add`, `get(x)` (0 for unseen), `items()`, `mostcommon(n=None)`, `_getitem_` (`c[x]`,
  0 for unseen), `_str_` (`Counter({...})`).
- `Counter(iterable)` tallies elements; over a String it counts code points.
- `mostcommon`:
  - highest count first; `mostcommon(n)` returns `pairs[0:n]`.
  - `mostcommon(0)` → `[]`.
  - **`mostcommon(-1)` is an end-slice** (`pairs[0:-1]` = all but the least-common), NOT `[]`.
    Docs explicitly warn about this; behaviour matches.
  - Tie order among equal counts is unspecified (backing Dict is unordered) — docs say don't rely on
    it; the test only asserts on unambiguous top-distinct counts.
- **No `elements`, no arithmetic operators** (`+`/`-`/`&`/`|`), and the name is `mostcommon`
  (no underscore), **not** `most_common`. Docs correctly reflect this reduced surface.

### defaultdict — PINNED
- Members: `_getitem_` (inserts `factory()` for a missing key, as a side effect), `_setitem_`,
  `_contains_` (`k in d`), `keys`, `values`, `items`, `_str_` (`defaultdict({...})`).
- A missing-key **read** mutates the dict (inserts the factory value) — verified `d["y"]` then
  `"y" in d` is True.
- Fresh `factory()` per missing key (a mutable default like `[]` is independent per key).
- **No `get`, no `_len_`** — confirmed absent (docs describe `d[k]`, `d[k]=v`, `in`, keys/values/items).
- Hardening: `factory` is a required arg — `defaultdict()` raises
  `"function missing required argument 'factory'"`.

---

## heapq

Min-heap over a plain List. Documented surface = actual surface.

- PINNED behaviour:
  - `heapify(items)` returns a **new** List and does **not** modify `items` (test asserts input
    unchanged). Smallest is at index 0.
  - Repeated `heappop` yields **ascending sorted** order (heap property).
  - `heappush` keeps the invariant; min bubbles to root.
  - `heapreplace(heap, item)` returns the **old** smallest even when `item` is smaller, then places
    the new item and sifts down.
  - `nsmallest(n, items)` / `nlargest(n, items)`:
    - `n > len` → all elements (ascending / descending respectively).
    - `n == 0` → `[]`.
    - **negative n → `[]`** for BOTH (`nlargest` has an explicit `n <= 0` guard; `nsmallest` gets it
      from its `while len(out) < n` loop). This deliberately differs from `Counter.mostcommon(-1)`'s
      end-slice — pinned so the divergence is intentional and tested.
  - `merge(lists)` merges a List of (sorted) Lists into one sorted List; empty / all-empty inputs → `[]`.
- Hardening (raise, catchable):
  - `heappop([])` → `"heappop from empty heap"`.
  - `heapreplace([], item)` → `"heapreplace on empty heap"`.
  - pushing a value that can't be ordered against existing elements (e.g. `"x"` into `[1,2]`) →
    `"type 'String' does not support this operator"`.
- **`heappushpop` is NOT implemented** and is **not documented** — confirmed absent via `hasattr`.
  (The audit task's checklist listed `heappushpop`, but neither the docs nor the impl provide it;
  this is a naming expectation in the task, not a doc/impl mismatch. Docs are correct as written.)

---

## bisect

Binary search / ordered insertion on a sorted List. Documented surface = actual surface.

- **Kirito names have NO underscores**: `bisectleft`, `bisectright`, `insortleft`, `insortright`,
  plus aliases `bisect` = `bisectright` and `insort` = `insortright`. The Python-style
  `bisect_left`/`insort_right` names do **not** exist (confirmed via `hasattr` — this matches the
  docs, which use the no-underscore spellings).
- PINNED behaviour:
  - `bisectleft` = leftmost valid insertion index; `bisectright` = rightmost. They differ across a
    run of duplicates (`[1,2,2,2,3,5]`: left(2)=1, right(2)=4) and coincide for a value in a gap.
  - Before all → 0; after all → `len(a)`; empty list → 0.
  - `insortleft`/`insortright` insert at the respective index (both keep the list sorted; they differ
    only in placement relative to equal keys).
  - Works on any comparable element type (Integers, Strings).
- Hardening (raise, catchable):
  - comparing incompatible types raises. `bisectleft` (`a[mid] < x`, Integer on the left) →
    `"unsupported operand type 'String' for comparison with 'Integer'"`; `bisectright`/`insortright`
    (`x < a[mid]`, String on the left) → `"type 'String' does not support this operator"`. Both
    surface a clear, catchable error rather than silently mis-inserting.

---

## Test coverage summary

| file | asserts | functioning | edge | hardening |
|------|---------|-------------|------|-----------|
| verify_collections.ki | 55 | deque/Counter/defaultdict full surface | empty, single, missing keys, negative mostcommon, fresh-per-key factory | empty pop/popleft, index oob, missing factory arg, absent-member probes |
| verify_heapq.ki | 31 | heapify/push/pop sort order, replace, nsmallest/nlargest/merge | n>len, n=0, negative n, empty, single | empty pop/replace, mixed-type push, no heappushpop |
| verify_bisect.ki | 28 | left/right insertion points, dup runs, insort, aliases, String keys | empty, before/after all, boundaries, gap | mixed-type compare, no underscored aliases |

No discrepancies to escalate to `DISCREPANCIES.md`.
