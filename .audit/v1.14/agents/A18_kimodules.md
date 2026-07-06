# A18: Kirito-authored frozen-source modules audit (v1.14)

Subsystem: src/kirito/stdlib_kimodules.hpp
Status: COMPLETE
Binary: /home/user/kiritolang.github.io/build-debug/ki

Modules: itertools, functools, collections, statistics, string, textwrap,
base64, csv, tabular, xml, heapq, bisect, copy, enum, tee, arg, semver.

## Bottom line
This is a very mature, heavily-audited subsystem. Prior rounds (r4–r11 audit
families + verify_* + spec_* + deep_* CTest scripts, plus test_semver.cpp,
test_kimods_deep.cpp, test_tabular_xml_deep.cpp) cover essentially every edge I
could construct. Every adversarial/edge probe I ran either behaved robustly or
turned out to be **explicitly pinned as intentional** in the test suite. I found
NO new correctness bugs. The items below are low/informational — divergences or
error-message nits that are, on inspection, already-known and mostly deliberate.

---

## Verified-robust (no action; recorded so the next round can skip)
- itertools: product/permutations/combinations result-guards hold; r=0 -> [[]],
  r>n -> []; empty pool -> []; accumulate custom fn OK; ziplongest fillvalue OK;
  takewhile/dropwhile/compress (selectors longer than data) OK; islice neg start
  accepted silently (A23-4, known).
- functools: reduce empty throws cleanly, None seed honored (identity sentinel),
  single element returned; cache memoizes wrapper but not internal recursion
  (A23-10, known); partial/starmap do not spread (A23-12, known).
- collections: Counter.mostcommon(-1) end-slice IS INTENTIONAL and pinned
  (verify_collections.ki:98, r8_kimods_a.ki:202) — do NOT report as a Python
  divergence; defaultdict factory-mutation works, no _len_ (A23-8, known).
- statistics: variance n<2 throws, mode empty throws, quantiles n=1 -> [],
  multimode ties OK, mean of mixed int/float uses Float accum (A24-1 fix holds).
- string: similarity/closest/fuzzymatch on empty list/string OK.
- base64: binary Bytes round-trip OK; urlsafe OK; decode length/leftover guards OK.
- csv: RFC-4180 quoting — embedded comma/quote/newline in quoted fields parse and
  format round-trip correctly.
- xml: 5000-deep nest parses/itertext/tostring iteratively (no stack overflow);
  named + numeric (&#dd; &#xHH;) entities, CDATA, comments, <?xml?> decl, unquoted
  attrs (-> "") all handled; surrogate numeric refs kept verbatim (v1.13 holds);
  mismatched/unclosed tags pop generically without crashing (audit_xml.ki:97, known).
- heapq: heapify/heappop full-sort invariant OK; nlargest/nsmallest non-positive
  n -> [] (pinned); heapreplace/heappop empty throw.
- bisect: left/right, empty, all-equal, insort OK.
- copy: deepcopy of a cycle terminates and preserves self-reference; shared refs
  preserved and de-aliased from original; native value type (Matrix) deep-copies
  independently.
- enum: dup-name rejected (v1.13 holds), names()/values() in definition order,
  _contains_ OK; nameof(unknown) raw Dict error (A23-9, known).
- tee: fan-out to BytesIO OK; write to a non-writable copy stream propagates a
  clean attribute error.
- semver: partial-comparator round-up (>1, >1.2, <=1.2) correct (v1.13 holds);
  ^/~/x-range/hyphen/whitespace-comparator/maxsatisfying all correct; prerelease
  precedence (num < alpha, pre < release) correct; build metadata ignored;
  malformed parse -> valid()==None; inc("prerelease") THROWS **by design**
  (verify_semver.ki:178, r7_kimods_b.ki:600) — do NOT report as a gap.

---

## Findings (all low / informational)

### A18-1: merge triple-suffix collision throws an opaque length error (message-quality nit)
- severity: info (behavior itself is pinned/wanted)
- location: stdlib_kimodules.hpp `_merge` (~L2140-2145 suffix logic) -> DataFrame
  `_fromcolumns` (~L2668)
- category: diagnostics / error-message clarity
- description: When a value column's auto-suffixed output name (`col_x`/`col_y`)
  collides with a column already literally named that (e.g. left has `a` and
  `a_y`, right has `a` -> right's `a` renamed to `a_y`, duplicate), `outcols`
  carries a duplicate name; the backing Dict can't hold both, and construction
  fails with `DataFrame: all columns must have the same length`.
- failure-scenario: `merge` of frames whose suffixing produces a name already
  present. Confirmed to throw (q4 probe).
- proposed-test: n/a — already pinned in r8_tabular.ki:65 ("merge throws when the
  _y suffix collides with an existing left column"). The behavior (throw) is the
  intended one.
- proposed-fix: OPTIONAL — before constructing, detect a duplicate in `outcols`
  and throw a targeted `merge: output column name 'a_y' already exists` so the
  user knows to rename/drop, instead of the misleading length message.
- confidence: high (confirmed); value low (message-only).

### A18-2: csv.format renders None as the literal "None"
- severity: info
- location: stdlib_kimodules.hpp csv `formatrow` (~L678, `var s = String(f)`)
- category: correctness / API divergence
- description: `csv.format([[None, "b"]])` -> `None,b`. `formatrow` does
  `String(f)` on every field, so a None cell serializes to the text `None`
  rather than an empty field. tabular.DataFrame.tocsv special-cases this
  (`"" if v == None else String(v)`, L1970), so the divergence only surfaces when
  a caller feeds None straight to the low-level csv module.
- failure-scenario: hand-built rows with None cells round-tripped through
  csv.format -> csv.parse come back as the string "None", not "".
- proposed-test: `assert csv.format([[None,1]]) == ",1"` (if the empty-field
  convention is adopted) — currently untested.
- proposed-fix: OPTIONAL — emit an empty field for None in formatrow, or document
  that csv is byte-literal and callers must pre-stringify. Since csv is documented
  "low-level", documenting is sufficient.
- confidence: high (confirmed); value low.

### A18-3: heapq.nlargest/nsmallest have no key= parameter
- severity: info (scope/parity gap, not a bug)
- location: stdlib_kimodules.hpp heapq `nsmallest` L812 / `nlargest` L819
- category: feature parity
- description: Python's heapq.nlargest/nsmallest accept `key=`. The Kirito versions
  take only `(n, items)` and rank whole elements, so `nlargest(2, [[1,"a"],[3,"b"]])`
  compares the sub-lists lexicographically. No key-based selection is possible.
- failure-scenario: a caller wanting "top-n by field" must pre-map or sort
  themselves. Not a crash; just an absent feature.
- proposed-test: n/a (feature request).
- proposed-fix: OPTIONAL — add an optional `key=None` to both, mirroring the
  `sorted`/`min`/`max` key protocol.
- confidence: high; value low.

### A18-4: statistics rejects Bool while tabular treats Bool as 0/1 (cross-module inconsistency)
- severity: info (both sides pinned/intentional)
- location: statistics `mean`/`median`/`mode` (L373-405) vs tabular `_numeric`
  (L1293-1302)
- category: consistency / documentation
- description: `statistics.mean([True,False])` throws (Bool not an arithmetic
  operand; pinned verify_statistics.ki:86) and `statistics.median`/`mode` throw
  via `sorted` ("cannot order 'Bool'"; pinned :87). Meanwhile `tabular.Series`
  reductions treat Bool as 0/1 (sum([True,True,False]) == 2, mean == 0.667, per
  CLAUDE.md). Two numeric-reduction surfaces in the same subsystem disagree on Bool.
- failure-scenario: a user who learns Bool-as-0/1 from tabular is surprised that
  statistics.mean of the same data raises.
- proposed-test: n/a (both behaviors already pinned).
- proposed-fix: OPTIONAL — a one-line doc note in the statistics reference that,
  unlike tabular, it does not coerce Bool. (Changing statistics to coerce is a
  larger call; the PIN says the current raise is deliberate.)
- confidence: high; value low.

---

## Coverage-gap sub-task
C++ (`test_semver.cpp`, `test_kimods_deep.cpp`, `test_tabular_xml_deep.cpp`,
`test_collections_deep.cpp`, `test_stdmodules.cpp`, `test_stdlib_gaps.cpp`,
`test_audit_findings.cpp`) + the large `.ki` script families (verify_*, spec_*,
deep_*, audit_*, r4_/r6_/r7_/r8_/r9_/r10_/r11_kimods*) together give this
subsystem excellent coverage. Specific edges I checked and found ALREADY covered:
- Counter.mostcommon negative n (verify_collections.ki, r6/r7/r8/r10_kimods).
- semver.inc unsupported release (verify_semver.ki:178, r7_kimods_b.ki:600).
- heapq nlargest negative/zero n (test_audit_findings.cpp:177-179).
- xml mismatched/unclosed/stray-close tags (audit_xml.ki, spec_xml.ki, verify_xml.ki).
- merge _x/_y collision AND triple-collision-throws (deep_tabular.ki, r7/r8_tabular.ki).
- statistics Bool raises (verify_statistics.ki:85-87).

Minor untested edges (all low-value): csv.format(None) text (A18-2);
heapq.nlargest with list elements / no key (A18-3). No coverage gap of consequence.

## Summary
- New correctness bugs: 0
- Informational / divergence / parity notes: 4 (A18-1 message nit, A18-2 csv None,
  A18-3 heapq key, A18-4 statistics-vs-tabular Bool)
- Verified-intentional (would-be false positives caught by reading the tests):
  Counter.mostcommon(-1), semver.inc("prerelease"), merge triple-collision throw,
  statistics Bool raise, xml lenient mismatched tags.
- Assessment: subsystem is solid and thoroughly tested; low-yield audit.
