# A14 — Kirito-authored frozen-source stdlib modules

Scope: `src/kirito/stdlib_kimodules.hpp` (itertools, functools, collections, statistics,
string, textwrap, base64, csv, tabular, xml, heapq, bisect, copy, enum, tee, arg, semver).

Findings below. Each verified against `./build-debug/ki`.

### F14-1 [Med] statistics.quantiles: tail cut-points wrong (clamps value instead of extrapolating index)
- stdlib_kimodules.hpp:459-480 — `quantiles` (exclusive method). When a cut position `i*(ld+1)/n`
  falls below 1 or at/above `ld`, the code clamps to the *endpoint value* (`s[0]` / `s[ld-1]`).
  CPython's `statistics.quantiles` clamps the *index* j into `[1, ld-1]` but keeps interpolating with
  the (possibly negative or >n) delta, i.e. it EXTRAPOLATES past the data at the tails.
  / Trigger: `quantiles([10,20,30,40,50], 10)` → Kirito `[10.0,12.0,...,50.0]`, Python
  `[6.0,12.0,...,54.0]`; `quantiles([1,2], 4)` → Kirito `[1.0,1.5,2.0]`, Python `[0.75,1.5,2.25]`.
  Middle cut-points agree; only the extrapolation regime (n large relative to ld, or extreme cuts)
  diverges. Silent wrong statistical output, no error.
  / Fix idea: mirror CPython — `j = clamp(Integer(i*(ld+1)/n), 1, ld-1)`, `delta = i*(ld+1) - j*n`,
  `result = (s[j-1]*(n-delta) + s[j]*delta)/n`.
  / Test to add: `spec` asserting `quantiles([1,2],4)==[0.75,1.5,2.25]` and a decile case.
  / Verified-real: CONFIRMED (diffed against CPython statistics.quantiles).


## tabular + remaining modules (re-run continuation)

### F14-2 [Med] collections.Counter.mostcommon(n) with NEGATIVE n returns an end-slice, not []
- stdlib_kimodules.hpp:345-349 — `mostcommon(n)` does `return pairs[0:n]`. With a negative n this is a
  Kirito end-slice: `mostcommon(-1)` returns ALL-but-the-last pair. CPython's
  `Counter.most_common(n)` is `heapq.nlargest(n, ...)`, which returns `[]` for any n <= 0.
  / Trigger: `Counter(["a","a","b","b","b","c"]).mostcommon(-1)` -> Kirito `[['b',3],['a',2]]`
  (Python `[]`). Confirmed via ./build-debug/ki.
  / Fix: `if n != None and n < 0: return []` (or `n = _max(n, 0)` before slicing) — mirror heapq/nlargest.
  / Verified-real: CONFIRMED. Silent wrong result, no error.

### F14-3 [Low] collections.deque lacks maxlen and rotate (common deque surface missing)
- stdlib_kimodules.hpp:303-328 — `deque` implements append/appendleft/pop/popleft/len/getitem/iter but
  has NO `maxlen` (bounded ring buffer) and NO `rotate`. `deque([...], maxlen=3)` throws "'_init_' takes
  1 positional argument(s) but 2 were given"; `d.rotate(1)` throws "no attribute 'rotate'". These are the
  two most-used deque features beyond the basic ends. CLAUDE.md's audit brief names "deque maxlen+rotate"
  as expected surface, and the module doc-comment advertises deque as a `collections.deque` analogue.
  / Trigger: `import("collections").deque([1,2,3,4,5], 3)` / `d.rotate(1)`. Confirmed.
  / Fix: add a `maxlen` kwarg (evict from the far end on append/appendleft when full) and a `rotate(n=1)`.
  / Verified-real: CONFIRMED (feature/coverage gap, not a wrong-answer bug).

### F14-4 [Med] DataFrame-from-rows silently TRUNCATES a too-long later row (data loss); too-short throws a raw diagnostic
- stdlib_kimodules.hpp:1722-1738 (`_fromrows` positional path) — column count is fixed from `rows[0]`
  only. A LATER row that is LONGER than the first is silently truncated to the first row's width (its
  trailing cells vanish); a SHORTER later row throws the raw native `"index out of range"` from
  `r[ci]` (line 1735), not a tabular diagnostic. This is inconsistent with `readcsv`, which explicitly
  REJECTS a too-long row (line 2281 "readcsv: row N has M fields, expected K") and pads a short one — so
  the two row-ingest paths disagree on ragged data.
  / Trigger: `DataFrame([[1,2],[3,4,5]]).torows()` -> `[{col0:1,col1:2},{col0:3,col1:4}]` (the 5 is
  dropped, no warning); `DataFrame([[1,2],[3]])` -> throws bare "index out of range". Both confirmed.
  / Fix: validate every row's width against the first (throw a clear "row i has W fields, expected N"),
  or pad-short/throw-long to match readcsv — one shared rule.
  / Verified-real: CONFIRMED. The truncation is silent data loss; the short-row message is unactionable.

### F14-5 [Low] tabular merge / GroupBy.agg surface raw Dict "key not found" instead of a domain diagnostic
- stdlib_kimodules.hpp:2150/2200 (`_merge` reads `right.data[on]` / `left.data[on]`) and 2123
  (`agg` does `reducers[spec[c]]`). When `on` names a column absent from a frame, or `spec` names an
  unknown reduction, the error is the bare Dict lookup `"key not found: <name>"` — not "merge: column
  'x' not found in right frame" or "agg: unknown reduction 'bogus'".
  / Trigger: `merge(l, r, "zzz")` -> "key not found: zzz"; `gb.agg({"x":"bogus"})` -> "key not found:
  bogus". Both throw (no silent wrong result), just with an opaque message. Confirmed.
  / Fix: validate `on in left.columns`/`on in right.columns` and `spec[c] in reducers` up front with a
  named message.
  / Verified-real: CONFIRMED (diagnostic quality, not correctness).

## Confirmed-CLEAN (tested, no defect)
- Series arithmetic: Series-Series length mismatch THROWS (no silent asymmetric truncation); scalar
  broadcast, missing (None/NaN) propagation to None, all correct.
- Series aggregations sum/mean/min/max/median/std on empty & all-NaN return 0/None correctly; Bool as 0/1.
- readcsv/tocsv round-trip is byte-stable incl. empty fields->None->"" and 0x-prefixed cells kept as text.
- DataFrame boolean-mask misalignment THROWS ("boolean mask length does not match row count"); iloc OOB,
  loc missing label, df+df all throw clear errors; describe on non-numeric/empty frames -> empty, no crash.
- groupby.sum/agg, NaN-key drop, None-key keep — all correct.
- csv module: quoting of embedded comma/quote/newline round-trips exactly (RFC-4180).
- xml: named + numeric (dec/hex) entities, CDATA (raw), comments, XML decl, unknown/malformed entity kept
  verbatim, itertext + tostring round-trip — all correct; lenient, never crashes.
- semver: ^/~/x-range/hyphen/AND/OR, prerelease precedence + gating, satisfies/maxsatisfying/sort — a
  20-case battery all correct (matches node-semver).
- statistics: multimode, mode/mean/variance/stdev on empty/single/all-equal all correct (throw where due).
- copy.deepcopy of a self-referential cycle + shared refs preserved; heapq nsmallest/nlargest/merge with
  negative n -> []; functools.reduce sentinel-by-identity; bisect; enum dup-name rejection — all correct.
