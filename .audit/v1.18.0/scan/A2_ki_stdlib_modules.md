# A2 — Kirito-implemented stdlib modules (stdlib_kimodules.hpp)

Full read + adversarial probe of every module on build-debug/ki; numeric cross-check vs python3.

## Findings
- MED — `Series.astype()` silently coerces to String on any unknown dtype (`conv = String` default,
  only overridden for Integer/Float/Bool). `astype("int")` → strings. → FIXED (#2).
- MED — `DataFrame(...)` does not validate `index` length; Series does. Mismatch → corrupt frame,
  later `index out of range`. → FIXED (#3).
- LOW — `DataFrame.describe()` with no numeric columns → inconsistent 0-row/6-label frame. → FIXED (#4).
- LOW — `GroupBy.agg` bad reducer/column → bare `key not found`. → FIXED (#5).
- INFO — collections header comment claims a nonexistent `OrderedDict`. → FIXED (#10).

## VERIFIED CORRECT
statistics (mean/median/variance/stdev/mode/multimode/quantiles vs CPython, incl. extrapolation);
itertools (permutations/combinations/product order+values, accumulate, groupby, pairwise, islice,
compress, eager-size guards); collections.deque (two-stack half-steal under alternating pop/popleft),
heapq (nsmallest/nlargest incl. n≤0/ties, k-way merge deterministic tie-break); base64 (all 256 bytes,
padding-less, urlsafe, strict rejection); csv (RFC-4180 quoting, embedded commas/quotes/newlines, bare
quote literal, CRLF); semver (^/~/x-range/hyphen/comparators, prerelease precedence + gating,
maxsatisfying, sort); xml (entity (un)escape, CDATA, comments/PI/doctype skip, lenient auto-close);
copy/deepcopy (shared refs, cycles, cross-boundary shared instance — the prior cross-boundary bug is
fixed); arg; tabular (position-aligned arithmetic + NaN propagation, duplicate-column rejection,
ragged-row rejection, NaN-key drop + None-key keep in groupby/merge, merge inner/left/right/outer with
_x/_y suffixing, valuecounts/sortvalues stable na-last); functools/enum/bisect/textwrap/Counter/
defaultdict/tee.

Note: the NaN-key rejection (#7) required NaN-safe fixes to `Series.unique`/`isin` (they keyed column
values into a Dict/Set); `unique` now collapses NaN to ONE distinct value (matching pandas) and `isin`
skips a NaN query value (never matches anyway).
