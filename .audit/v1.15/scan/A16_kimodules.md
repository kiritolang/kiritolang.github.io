# A16 Frozen .ki modules

Status: IN PROGRESS -> COMPLETE

Scope: src/kirito/stdlib_kimodules.hpp (itertools, functools, collections, statistics, string,
textwrap, base64, csv, tabular, xml, heapq, bisect, copy, enum, tee, arg, semver). Read-only scan;
findings below, confirmed against build-debug/ki where noted. Consulted .audit/README.md's
false-positive table first (semver leading-zero leniency, tabular NaN-key drop, tabular ragged-row
truncation, statistics.quantiles clamp, base64 whitespace rejection — none re-flagged here).

Coverage reviewed: tools/tests/unit/test_tabular.cpp, test_xml.cpp, test_stdmodules.cpp,
test_extras.cpp, test_kimods_deep.cpp, and tools/tests/scripts/{r7,r8,r9,r10,r11,deep,labx,verify,
audit,probe,spec}_*.ki touching these modules.

---

### A16-1: DataFrame(rows, columns=...) with a column-count mismatch: asymmetric, unfriendly failure modes  [MEDIUM] [confirmed]
- **Location**: `stdlib_kimodules.hpp`, `tabular` module, `DataFrame._fromrows` (list-of-rows,
  non-Dict-row branch), around the `while ci < len(cols): ... col.append(r[ci]) ...` loop.
- **What**: When constructing a `DataFrame` from a List of same-width rows plus an explicit
  `columns=` list whose length does **not** match the row width:
  - fewer column names than the row width: the loop only iterates `len(cols)` times, so trailing
    fields in every row are **silently dropped** — no error, no warning, just quiet data loss.
  - more column names than the row width: `r[ci]` indexes past the end of each row once `ci >=
    len(row)`, throwing a raw, module-internal **"index out of range"** error (via `<tabular>:436`)
    instead of a clear `DataFrame`-specific diagnostic naming the mismatch.
  This is a different scenario from the already-accepted "ragged row" false positive (pinned in
  `.audit/README.md`), which is about row-to-row width variance against an inferred/CSV-header
  column count. Here the mismatch is against the caller's own explicit `columns=` argument, and the
  two failure directions (silent truncation vs. an opaque native error) are inconsistent with each
  other and with the rest of the module's "throw a clear message on shape mismatch" convention (see
  `Series` index-length check, `DataFrame._mask` mask-length check, `_fromcolumns` column-length
  check).
- **Repro** (confirmed against `build-debug/ki`):
  ```
  var tb = import("tabular")
  var io = import("io")
  # fewer columns than row width: column "c" (3rd field of every row) silently vanishes
  var df1 = tb.DataFrame([[1,2,3],[4,5,6]], columns=["a","b"])
  io.print(df1.columns)        # ['a', 'b'] -- no error, no hint data was dropped

  # more columns than row width: raw index-out-of-range from inside the module
  var df2 = tb.DataFrame([[1,2],[3,4]], columns=["a","b","c"])
  # -> <tabular>:436:33: error: index out of range
  ```
- **Proposed fix**: in `_fromrows`'s non-Dict branch, before the `ci` loop, check
  `len(cols) == len(rows[0])` (or per-row) and `throw "DataFrame: columns length (" + String(len(cols))
  + ") does not match row width (" + String(len(rows[0])) + ")"` — mirrors the existing
  `_fromcolumns` message style ("DataFrame: all columns must have the same length"). This closes the
  silent-truncation path and replaces the raw index error with a clear one.
- **Proposed test**: add to `tools/tests/unit/test_tabular.cpp`'s adversarial block:
  `bad("pd.DataFrame([[1,2,3],[4,5,6]], columns=[\"a\",\"b\"])")` and
  `bad("pd.DataFrame([[1,2],[3,4]], columns=[\"a\",\"b\",\"c\"])")`.

---

### A16-2: heapify / nsmallest are O(n log n) via repeated heappush, not O(n) sift-down heapify  [LOW] [confirmed by reading]
- **Location**: `stdlib_kimodules.hpp`, `heapq` module, `heapify`.
- **What**: `heapify(items)` builds the heap by calling `heappush` (an O(log n) sift-UP) once per
  item, giving O(n log n) instead of the classical O(n) bottom-up sift-DOWN heapify. Purely a
  performance/DRY note (the module already has `_siftdown`, used correctly by `heappop`/
  `heapreplace`), not a correctness bug — `nsmallest` inherits the same cost via `heapify`.
- **Repro**: N/A (perf characteristic, not incorrect output).
- **Proposed fix**: rebuild `heapify` bottom-up: append all items unsifted, then for
  `i` from `n//2 - 1` down to `0` call a sift-down rooted at `i` (needs a `_siftdownfrom(heap, i)`
  generalizing `_siftdown`, which currently hardcodes root 0). Optional — flag for the perf pass,
  not semantics.
- **Proposed test**: N/A (would need a perf/op-count assertion, not a correctness test).

---

### A16-3: COVERAGE GAP roll-up (untested edges, prioritized per the tasking)

None of these reproduced a wrong answer when spot-checked (see notes); listed as gaps in the
`.ki` golden-script suite for a future round to pin down explicitly, since the current suite is
otherwise strong (r4–r11 kimods scripts, `verify_*`, `deep_*`, `audit_*`, `labx_*`, `spec_*`,
`probe_*` families):

- **itertools**: `islice` with `step` not dividing evenly against `start`/`stop` combined with a
  non-1 `start` (`islice(range(20), 3, 17, 4)`) — spot-checked correct, no golden test pins the exact
  emitted indices for a non-trivial `(start, step)` pair together.
  `product([])` (zero pools) — returns `[[]]` (the multiplicative identity); not asserted anywhere.
  `groupby` with a `key` function that raises inside the loop — untested error path.
- **tabular**: `Series([])._binop` against a scalar (0-length arithmetic) is untested; `DataFrame`
  built from `data=None, columns=[...]` (columns given but no data/index) is untested — spot-check
  shows `self.columns` stays `[]` (the `columns` ctor arg is silently ignored in the `data == None`
  branch, only `index` is honored) — a smaller instance of the A16-1 family, worth folding into the
  same fix. `GroupBy.agg` with an unknown column name in `spec` (not just an unknown reducer name,
  which IS tested) is untested. `merge` when the `on` column has a different **type** between the two
  frames (e.g. Integer id in one, String id in the other) — every value is simply non-matching
  (no exception, no coercion) — not asserted either way.
- **xml**: an attribute value containing an unescaped `&` followed by no `;` before the tag ends
  (`<a x="1 & 2">`) — the `_parse_tag` value scanner runs to the closing quote regardless, then
  `_decode` on the raw text finds `&` with no `;` within the *whole remaining string* (not bounded to
  the attribute) since `s.find(";", i)` searches to the end of `s` — for an attribute value this `s`
  is already just the sliced attribute text, so it's fine; not a bug, just worth an explicit golden
  test since the interaction between `_parse_tag`'s quote-scanning and `_decode`'s entity-scanning
  across a `&`-without-`;` is subtle enough to regress silently.
- **semver**: `satisfies` against an explicit-prerelease comparator whose core version differs from
  the tested version but is otherwise satisfied by every numeric comparator (node-semver's
  same-triple-only prerelease-matching rule) has only light coverage; `_parserange` hyphen-range
  combined with an OR (`"1.0.0 - 2.0.0 || 3.0.0"`) is untested. `maxsatisfying`/`minsatisfying` with an
  empty `versions` list — spot-checked returns `None`, untested explicitly.
- **csv**: `parse` of a lone `"` (single unterminated quote, nothing else) — spot-checked doesn't
  crash (falls off the end of the string still `inQuotes`, appends whatever was buffered), but no
  golden test pins this exact "unterminated quote at EOF" shape.
- **copy.deepcopy**: cyclic *Dict* and cyclic *List* are both tested (`verify_copy.ki`); a cycle
  running through a **Set** is not reachable at all (Sets can only hold hashable elements, and a
  container can't be a Set member), so this is a non-issue, not a gap — noting it here only so a
  future round doesn't waste time chasing it.

---

## Summary

One confirmed, fixable bug (A16-1): `DataFrame(rows, columns=...)` silently drops trailing row data
when `columns` is shorter than the row width, and throws a raw, unfriendly "index out of range"
(rather than a clear DataFrame-level message) when `columns` is longer — both directions are
untested and inconsistent with the module's otherwise-consistent "throw a clear message on a shape
mismatch" convention (`Series` index-length, `DataFrame._mask`, `_fromcolumns`). A related, smaller
instance was spot-checked in the same constructor: `DataFrame(data=None, columns=[...])` silently
ignores the `columns` argument entirely. One low-severity DRY/perf note (A16-2: `heapify` is O(n log
n) via repeated `heappush` instead of an O(n) bottom-up sift-down, despite `_siftdown` already
existing). No other logic bugs were confirmed by direct execution across itertools, functools,
collections, statistics, string, textwrap, base64, csv, xml, heapq, bisect, copy, enum, tee, arg, or
semver — the existing golden-script suite (r4–r11, verify/deep/audit/labx/spec/probe families) is
unusually thorough for this subsystem and already pins most of the adversarial cases suggested in
the tasking (itertools combinations/permutations r>n/r=0, statistics variance/mode/quantiles edges,
csv quoted/embedded fields, base64 padding, semver range grammar, xml malformed/mismatched tags,
copy.deepcopy cycles). A16-3 lists untested-but-not-wrong edges for a future round to pin explicitly.
