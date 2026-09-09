# A8 — Kirito-implemented stdlib (`src/kirito/stdlib_kimodules.hpp`) correctness/idiom audit

**Round:** v1.17.1 · **Scope:** the ~3200-line embedded-Kirito stdlib (all modules below), *excluding*
the tabular Series/DataFrame slicing semantics (audited separately). Slice audit re-check was not
repeated here.

**Method:** read the full Kirito source of every module. Reproduced behaviour on the real
`build-bin/ki-release` binary (asan spot-checks available; nothing needed one). Every numeric result was
recomputed by hand / against CPython's actual algorithm. Probes written to `/tmp/*.ki`.

**Prior round:** commit `d1c0800` deep-audited this same file. This pass re-probed freshly and
adversarially for anything missed or newly exercised.

---

## Headline

**No CONFIRMED logic, numeric, complexity, or silent-failure bugs found.** Every module I exercised
produced correct results verified by independent hand computation, correct error behaviour on
adversarial/edge input, and the advertised complexity. The prior-round hardening (`d1c0800`) holds up
under fresh adversarial probing. Findings below are all INFORMATIONAL (documented-by-design behaviour),
not defects.

---

## Findings (all INFORMATIONAL — no defects)

### I1 — `statistics.quantiles` clamp+extrapolate: initially looked wrong, is CORRECT
- **statistics** (lines 492–516). `quantiles([1,2], n=4) == [0.75, 1.5, 2.25]` — cut points fall
  *outside* the data range `[1,2]`. I first suspected this diverged from CPython. It does **not**:
  CPython's `statistics.quantiles` (exclusive method) computes `delta = i*m - j*n` **after** clamping
  `j`, which is exactly what this code does — so CPython yields the identical `[0.75, 1.5, 2.25]`.
  Verified by hand for `[1,2],n=4` and `[1,2,3,4],n=4` → `[1.25,2.5,3.75]` (no clamping there). The
  docstring/comment and `docs/pages/10-stdlib.md:1386-1389` both document the extrapolation accurately.
  **CONFIRMED CORRECT.**

### I2 — `textwrap.wrap` is a deliberate simplification (not CPython-equivalent)
- **textwrap** (lines 588–602). `wrap` splits only on a single `" "`, does **not** break words longer
  than `width` (`wrap("a supercalifragilistic word", 8)` → `['a','supercalifragilistic','word']`), and
  does **not** collapse whitespace runs or treat `\t`/`\n` as separators (`wrap("a  b   c",10)` →
  `['a  b   c']`). This differs from CPython `textwrap`, but `docs/pages/10-stdlib.md:1851-1856`
  describes it minimally and claims no CPython parity, so it is documented-by-omission, not a
  doc-vs-reality bug. Severity: informational.

### I3 — `tabular.Series.median` returns Integer for all-integer, odd-length data
- **tabular** (lines 1633–1640). `Series([3,1,2]).median() == 2` (Integer), where pandas returns
  `2.0`. Even-length path divides so returns Float. Cosmetic type nuance; no wrong value. Informational.

### I4 — `tabular.Series.astype` throws on missing values
- **tabular** (lines 1699–1707). `Series([1,None,3]).astype("Integer")` throws
  `cannot convert 'None' to Integer` rather than propagating NaN like pandas. This is an *explicit,
  catchable* error (no silent corruption), consistent with the module's "no silent failure" stance.
  Informational, arguably correct-by-policy.

---

## Verified CORRECT (per module)

**itertools** — `count` (half-open, fwd/bwd; ternary-in-`while` precedence parses correctly),
`repeat`, `cycle`, `chain`, `islice` (start/stop/step), `accumulate`, `product`/`permutations`/
`combinations` (exact lexicographic CPython order + eager-size guards), `takewhile`, `dropwhile`,
`filterfalse`, `compress`, `starmap` (tuple-as-one-arg convention, consistent with `partial`),
`pairwise`, `ziplongest`, `groupby` (consecutive-key grouping).

**functools** — `reduce` (identity-sentinel handling of an explicit `None` seed; empty-no-init throws),
`partial` (snapshot semantics), `cache` (memoizes; verified call count = 2 for `5,5,6`).

**collections** — `deque` two-stack: correct interleaved append/appendleft/pop/popleft, negative and
in-range indexing, `_tolist`/`_str_`; **timed FIFO churn 20k/40k/80k = 122/196/364 ms → ~linear,
amortized O(1) confirmed (not O(n²))**. `Counter` (counts, `mostcommon(n)` descending, `[]` default 0),
`defaultdict` (factory, `in`), naming.

**statistics** — `mean` (Float-space accumulation), `median` (odd/even), `mode`/`multimode` (first-seen
tie order, matches CPython), `variance`/`pvariance` (sample vs population, hand-checked on the classic
`[2,4,4,4,5,5,7,9]` → 4.5714.../4.0), `stdev`/`pstdev`, `quantiles` (see I1).

**string** — `capwords`, `similarity` (`kitten`/`sitting` → 4/7 = 0.5714), `closest` (earliest-tie),
`fuzzymatch`, constants. Ratio math correct.

**textwrap** — `wrap`/`fill`/`indent` (blank lines unprefixed), `dedent` (common-*string* prefix incl.
mixed tab/space → unchanged when no shared prefix). See I2.

**base64** — canonical vectors `M`→`TQ==`, `Ma`→`TWE=`, `Man`→`TWFu`, `hello world` round-trip;
whitespace-tolerant decode; rejects lone trailing char, data-after-padding, and out-of-range byte in
`encode`; url-safe `+/`↔`-_` translation + round-trip.

**csv** — RFC-4180 quoting, `""` un-escaping, embedded commas and newlines inside quotes, single shared
`_parserows` core for `parse`/`parserow`, round-trippable Float via `repr()`.

**heapq** — `nsmallest`/`nlargest` (n>len, n≤0), `merge` (k-way, deterministic tie-break via
`[value,li,ei]`), `heapify` (Floyd, returns new list), `heappush`/`heappop`/`heapreplace`. O(log n)
structure by construction.

**bisect** — `bisectleft`/`bisectright` on duplicate runs (`[1,2,2,2,3]` → 1/4), `insort*`, aliases.

**copy** — `deepcopy` preserves shared refs (both children same id), self-cycles, dict/set/list deep
independence; shallow `copy` shares inner containers; immutable no-op path.

**enum** — `get`/`nameof`/`names`/`values` (0..n-1, definition order), `in`, duplicate-name rejection.

**tee** — `tee_stdout` fan-out + restore, `Tee` pure-fanout sink, write ordering (copies then primary).

**arg** — positionals, `--opt val`, `--opt=val`, `-x` short (letter-only; negatives stay positional),
`--` end-of-options, type conversion from default type, `rest` collection, unknown-option errors.

**tabular** — Series aggregations (`sum/mean/count/median/std/min/max/prod` with None-skipping and
non-numeric HARD error), None-propagating `_binop`/comparisons/masking, `valuecounts`, `unique`;
DataFrame `sum/mean/min/max/std/count/describe`, boolean mask, `groupby` (`sum/mean/size/agg`), `merge`
(inner/left/outer with `_x`/`_y` suffixing and NaN-key skipping), `concat` (column union),
`readcsv`; duplicate-column, ragged-row, and extra-CSV-field rejection all fire. Numeric results
hand-verified.

**xml** — parse tag/attrib/text/**tail**/children, `find`/`findall`/`findtext`/`get`/`itertext`,
`tostring` round-trip (self-closing `<child2 />`), entity decode (`&lt; &amp; &#65; &#x42;`), CDATA raw,
attribute escaping round-trip, lenient numeric-entity/surrogate rejection.

**semver** — full precedence chain `alpha < alpha.1 < alpha.beta < beta < beta.2 < beta.11 < rc.1 <
release` (matches semver.org example), numeric-vs-alnum identifier rule, build metadata ignored in
`compare`; ranges `^`/`~`/x-range/`*`/hyphen/`||`/space-separated-operator (`>= 1.2.0`),
`satisfies`/`maxsatisfying`/`minsatisfying`, prerelease gating (only same-tuple pins allow a
prerelease), `sort`/`rsort`, `inc`/`diff`/`valid`/`validrange`/`clean`. Extensively hand-checked
against node-semver semantics.

---

## Honest nulls

I actively hunted for: divergent re-implementations, nsmallest-heap/nlargest-sort mismatch (they use the
same heap machinery — no mismatch), mislabeled sorts, wrong-file logic, lying comments, silent
column/key merging, aggregations dropping non-numeric data, parser junk-acceptance, base64 padding
corruption, deep-copy cycle sharing, tie-break inconsistency, and O(n²) hidden in a "fast" path.
**Found none.** The one thing that looked like a numeric divergence (quantiles, I1) turned out to match
CPython exactly once I traced CPython's real clamped-delta arithmetic.
