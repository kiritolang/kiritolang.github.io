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

### A18-2: `tabular` GroupBy.agg with an unknown reducer name leaks a raw "key not found"  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/stdlib_kimodules.hpp:2090-2107` (`GroupBy.agg`)
- What: `agg({col: name})` looks the reducer up with `reducers[spec[c]]` (a plain Dict index). A typo /
  unsupported name surfaces the *Dict's* internal error, not a helpful list of valid reducers. The doc
  comment right above even enumerates the legal names, but the code never validates against them.
- Repro:
```
var tb = import("tabular")
var d = tb.DataFrame({"g":[1,1,2], "v":[10,20,30]})
d.groupby("g").agg({"v": "bogus"})
```
  Real output:
```
agg bogus !! key not found: bogus
```
- Impact: a user misremembering a reducer name (`"average"`, `"total"`) gets an implementation-detail
  message with no hint of the allowed set. Same class as the deque/List leak (A18-1).
- Proposed fix: `if spec[c] not in reducers: throw "agg: unknown reducer '" + spec[c] + "' (use sum/mean/min/max/std/count/median)"`.
- Proposed test: assert the message names the bad reducer and lists valid ones.

### A18-3: `xml` unquoted attribute values become phantom attributes (value lost)  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/stdlib_kimodules.hpp:2489-2502` (`_parse_tag`, the attribute-value branch)
- What: after `aname=`, if the next char is not a quote the code leaves `aval=""` **and never advances
  `i` past the unquoted value**. The outer loop then re-reads that value token as the *next attribute
  name*. So `<a x=1 y=2>` yields attrib `{x:"", 1:"", y:"", 2:""}` — the real values are dropped and two
  bogus attributes (`1`, `2`) appear.
- Repro:
```
var xml = import("xml")
var q = xml.parse("<a x=1 y=2>t</a>")
q.attrib   # keys ['x','1','y','2']
```
  Real output:
```
attrib: {'x': '', '1': '', 'y': '', '2': ''}
```
- Impact: any HTML-style / hand-written tag with unquoted values (`<input type=text disabled>`,
  `<a x=1>`) silently corrupts the attribute map — values lost, spurious keys inserted. The parser
  advertises itself as lenient, but leniency should keep or skip the token, not fabricate attributes.
- Proposed fix: in the no-quote branch read the bare value token (`while i<n and not _isspace(s[i]) and s[i]!='>' and s[i]!='/'`) into `aval` so `attrib[aname]=aval` captures it and `i` advances past it.
- Proposed test: `xml.parse("<a x=1 y=2>t</a>").attrib == {"x":"1","y":"2"}` (or at minimum, no phantom `1`/`2` keys).

### A18-4: `enum.Enum.nameof` on an unknown value leaks a raw "key not found"  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/stdlib_kimodules.hpp:997-998` (`Enum.nameof`)
- What: `nameof(value)` does a bare `return self._byValue[value]`. Its sibling `get(name)` carefully
  throws `"no such enum member: ..."`, but `nameof` leaks the backing Dict's "key not found" for any
  value outside `0..n-1`. Asymmetric and internal-leaking (same class as A18-1/A18-2).
- Repro:
```
var en = import("enum")
var e = en.Enum(["RED","GREEN","BLUE"])
e.nameof(99)
```
  Real output:
```
nameof bad !! key not found: 99
```
- Impact: minor, but inconsistent with `get`'s clean diagnostic; the message names a raw Dict key, not
  an enum value.
- Proposed fix: `if value not in self._byValue: throw "no such enum value: " + String(value)`.
- Proposed test: assert the message says "no such enum value".

### A18-5: `tabular` Series comparison/arithmetic THROWS on a column with any missing value  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/stdlib_kimodules.hpp:1405-1417` (`Series._binop`, used by every `_add_`/`_sub_`/.../`_gt_`/`_lt_`/...)
- What: `_binop` applies `op(v, other)` to every element. When an element is `None` (a missing value —
  exactly what `readcsv` produces from an empty cell), `None > 26` / `None + 1` raises
  "type 'None' does not support this binary operator". So the DOCUMENTED masking idiom
  `df[df["col"] > v]` crashes whenever `col` has a single missing value. The rest of the library
  (sortvalues, groupby, merge, dropna, aggregations) all handle missing carefully — only the operators
  don't. pandas returns False for a NaN comparison (and NaN for NaN arithmetic), never an exception.
- Repro:
```
var tb = import("tabular")
var df = tb.readcsv("name,age\nAlice,30\nCarol,\n")   # Carol's age is missing
df[df["age"] > 26]
```
  Real output:
```
<tabular>:158:60: error: type 'None' does not support this binary operator
```
  (also `tb.Series([10,None,30]) + 1` and `> 15` both throw; `.eq(10)` works only because `None == 10`
  is legal.)
- Impact: boolean-mask filtering — a headline DataFrame feature — is unusable on any real CSV column
  that has a blank cell. A user must `dropna()`/`fillna()` first, undocumented and surprising.
- Proposed fix: in `_binop`, propagate missing rather than call `op`: `if _isnan(v) or _isnan(other): out.append(None)` else `out.append(op(v, other))`. A None mask entry is falsy, so `_mask` excludes that row (pandas parity); arithmetic yields None (a missing result), also pandas-consistent.
- Proposed test: `tb.Series([10,None,30]).__gt__(15)` (via `> 15`) must not throw and must produce a mask whose None row drops out of `df[...]`; `+1` on a Series with None must not throw.

> **A18-3 RETRACTED (not a finding):** `tools/tests/scripts/verify_xml.ki:136-138` explicitly pins this
> exact behavior — `xml.parse("<a x=1 y>t</a>").attrib == {"x":"","1":"","y":""}` with the comment
> "unquoted attr value not captured (lenient)". The maintainers deliberately chose lenient no-crash
> handling and froze the phantom-attribute output as the contract. It diverges from real XML/HTML
> parsers (which capture or reject an unquoted value), but it is intended, tested behavior, so I am
> withdrawing A18-3. Documented here for provenance so a future round doesn't re-flag it. (Candidate for
> the pinned-FP list in `.audit/README.md`.)

> **Note on A18-2 & A18-4 (scope = message quality only):** existing tests already pin that both THROW —
> `audit_tabular.ki:319` (`agg({"v":"BOGUS"})` throws) and `deep_kimods.ki:350` / `labx_datatools.ki:163`
> (`nameof(99)`/`nameof(5)` throws). Neither asserts the *message*. So A18-2/A18-4 are not "should throw"
> findings (that's settled) — they are narrowly about the leaked "key not found: X" text vs a clean,
> domain-specific message. Keep as LOW polish; a fix must not change that they throw, only the wording,
> and can pin the new message.

---

## Coverage gaps (most valuable first)

1. **tabular — Series/DataFrame ops on columns WITH missing values (A18-5).** Every masking test
   (`verify_tabular.ki:56,171-174,341-346`, `audit_tabular.ki`) uses fully-populated columns. No test
   exercises `series > scalar` / `series + scalar` when the column has a `None`/NaN — the exact case
   `readcsv` produces from a blank cell, and the case that throws. This whole failure mode is untested.
2. **tabular — GroupBy.agg / describe error-message quality (A18-2).** `audit_tabular.ki:319` pins that
   a bad reducer *throws*, but nothing pins the *message*; it currently leaks "key not found: X".
3. **tabular — non-numeric aggregation results.** `Series(["a","b"]).sum() == 0` and `agg({strcol:"sum"})`
   silently returns 0 for a string column (numeric filter yields empty → 0). No test asserts this; it is
   a silent wrong-looking answer (pandas concatenates strings). Borderline; documented as "numeric-only
   reductions", so recorded as an observation, not a hard finding.
4. **enum — nameof message quality (A18-4).** Tests pin that `nameof(unknown)` throws; none pin the
   message, which leaks "key not found".
5. **collections — Counter.mostcommon(negative n).** Returns `pairs[0:n]` (a Python-list slice = all but
   the last |n| entries) where CPython's `most_common(-1)` returns `[]`. Untested, minor divergence.
6. **collections — defaultdict surface.** No `get`, `_len_`, `_iter_`; `defaultdict(None)` (Python-legal,
   behaves as a plain dict) would throw `None()` on a missing key. Untested edges; likely by design.
7. **textwrap — long-word / multi-space handling.** `wrap` never breaks a word longer than `width` and
   never collapses runs of spaces (both pandas/CPython-divergent). Documented simplifications, untested
   as such.

## Non-findings (probed, correct)

- **itertools**: count(float/neg step), repeat(neg), cycle(0), chain, islice(neg→throws), accumulate(empty
  and with func), product([] → [[]], [[]] → []), permutations(r>n/neg/guard), combinations(r=0 → [[]]),
  takewhile/dropwhile, compress, ziplongest(uneven), pairwise(single→[]), starmap, groupby(consecutive +
  key). All correct.
- **functools**: reduce(empty→throws / with initial), reduce identity sentinel, partial(snapshot),
  cache(unhashable → clean "unhashable type 'List'"). Correct.
- **statistics**: variance(<2→throws), mode(empty→throws, tie→first-seen), quantiles(single→throws,
  n=1→[], default), median(mixed int/float), multimode. Correct.
- **semver**: valid/clean, prerelease precedence (alpha<beta, alpha.1<alpha.2, pre<release), ^/~/x-range/
  hyphen/OR ranges, maxsatisfying, validrange(garbage→False, ""→True), inc, diff. All correct.
- **base64**: encode/decode roundtrip, empty, urlsafe(+/→-_) roundtrip on high bytes. Correct.
- **csv**: quoted embedded newline, doubled-quote escaping, ragged rows, CRLF normalization. Correct.
- **heapq**: heapify(Floyd), nsmallest/nlargest, merge, heappop/heapreplace(empty→throw). Correct.
- **bisect**: bisectleft/right on duplicates, insort. Correct.
- **copy**: deepcopy(cyclic list, shared-ref dict, set), shallow copy shares inner, scalars pass through,
  instance deepcopy independent via serde. Correct.
- **enum**: get/nameof/names/values/contains, duplicate-name rejected, unknown member clean message.
  Correct (nameof message is the only nit, A18-4).
- **collections**: deque(appendleft/append/pop/popleft/negative index/iterate), Counter(most_common,
  [] default), defaultdict(factory). Correct apart from A18-1 and the mostcommon(-n) nit.
- **tee**: Tee(None) pure fan-out, write returns len, flush tolerant, fan-out to BytesIO. Correct.
- **arg**: positional/option/flag, inline `=`, short `-v`, negative-number positional, unknown option,
  missing positional, bad numeric, extra→rest. Correct.
- **string/textwrap**: capwords, similarity(scalar+list), closest(+empty→None), fuzzymatch, dedent,
  indent. Correct.
- **xml**: entities (named + numeric + verbatim-unknown), CDATA, comments, `<?xml?>`, mismatched/unclosed
  tags (lenient no-crash), 5000-deep nesting (iterative parse/serialize/itertext). Correct.
- **GC soak** (`--gc-threshold 1`) across tabular/itertools/copy/collections with integers >256 (outside
  the intern range): no crash, correct results.

Status: DONE
