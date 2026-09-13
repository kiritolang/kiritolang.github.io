# v1.18.0 — triaged findings

Severity: HIGH/MED/LOW/INFO. Status: FIXED (with regression test + docs) or noted. All CONFIRMED on a
real binary. No HIGH findings; no memory/UB/GC/race bug.

| # | Sev | Subsystem | Finding | Decision | Status |
|---|-----|-----------|---------|----------|--------|
| 1 | MED | net (C++) | HTTP gzip/deflate response decode skipped the CRC-32 / Adler-32 integrity check (corrupt-but-decodable body silently accepted); `net::gunzip` was a weaker duplicate of `gzipfmt::decompress` (SSOT). | Route `decodeBody` through the validating `gzipfmt::decompress` / `deflate::zlibDecompress`; drop the duplicate; keep the headerless-raw-DEFLATE interop fallback. | FIXED |
| 2 | MED | tabular (.ki) | `Series.astype(<unknown>)` silently coerced the column to String (docs enumerate only Integer/Float/Bool/String; `astype("int")` silently corrupts). | Validate the dtype name; throw a diagnostic on anything else. | FIXED |
| 3 | MED | tabular (.ki) | `DataFrame(index=…)` did not validate the index length (Series does); a mismatch built a corrupt frame that failed later with a misleading `index out of range`. | Add an SSOT `_indexfor` that validates length == row count, mirroring the Series check. | FIXED |
| 4 | LOW | tabular (.ki) | `DataFrame.describe()` with no numeric columns built a 0-row frame carrying 6 phantom stat labels in its index (inconsistent; broken once #3 validates). | Return a consistent empty DataFrame when there are no numeric columns. | FIXED |
| 5 | LOW | tabular (.ki) | `GroupBy.agg` with a bad reducer/column surfaced a bare Dict `key not found`. | Validate the spec up front with a diagnostic naming the op + valid set. | FIXED |
| 6 | MED | collections (C++) | Set `in`/`contains`/`discard` swallowed the unhashable-type error (returned False / no-op) that Dict `in` and Set `add`/`remove` raise — an SSOT + silent-failure gap. | Route the Set read paths through `requireHashable`; `discard` throws on unhashable too (only silent on *absence*). | FIXED |
| 7 | MED | collections (C++) | A NaN Dict/Set key was insertable but never findable/removable, and re-assignment fabricated a duplicate key — breaking the key-uniqueness invariant (old "write-only NaN" design). | **Reject** NaN keys at insert via `requireUsableKey` (a key not `==` itself). Fixed internal fallout in `tabular.unique`/`isin`. | FIXED |
| 8 | LOW | docs | The course doc called `\xHH` "a byte"; it is the code point U+00HH (UTF-8-encoded, so `\xff` is 2 bytes). | Reword to "code point"; point binary work at `Bytes`. | FIXED |
| 9 | MED | tensor (C++) | `Tensor.round()` used banker's rounding (half-to-even, `std::nearbyint`) while scalar `round()` rounds half-away-from-zero (`std::llround`) — silent disagreement, undocumented. | **Unify**: `Tensor.round()` uses `std::round` (half-away-from-zero) to match scalar `round()`. | FIXED |
| 10 | INFO | collections (.ki) | A comment claimed a nonexistent `OrderedDict` class. | Correct the comment (Dict is already insertion-ordered). | FIXED |
| 11 | LOW | net (C++) | `net.urlsplit` (the lenient informational splitter) accepts a malformed IPv6 `http://[::1/path` where Python `urlsplit` raises; never feeds a real request (those use the strict `parseUrl`). | Noted; no behavior change (documented-lenient splitter, no security impact). | NOTED |

## Regression tests added

- C++: `test_audit_regressions.cpp` (net gzip CRC-32 + zlib Adler-32 corrupt-trailer rejection, no
  server — corrupts only the trailer so the body still inflates, pinning integrity checking
  specifically); `test_collections_deep.cpp` (NaN key rejected on Dict+Set; Set in/discard/contains
  unhashable throw; NaN lookup absent-not-throwing); `test_audit_v1151.cpp` / `test_gc_generational.cpp`
  updated off the retired write-only-NaN premise.
- `.ki` error suite: `tabular_astype_unknown_dtype`, `tabular_dataframe_index_length`,
  `tabular_agg_unknown_reduction`, `set_in_unhashable`, `set_discard_unhashable`, `dict_nan_key`,
  `set_nan_element`.
- `.ki` goldens: `verify_tensor_round` (new); `audit_tabular` (astype/describe/agg asserts);
  behavior-change updates across `deep_tabular`, `deep_numeric`, `r4_tensor`, `r4/r8_kimods_a`,
  `verify_itertools`, `cov_collections_fp`, `r7_tabular`, `labx_stats_tabular`, `labx_containers`,
  `verify_types_collections`, `r7_types`, `r8_types`, `r9_types_err`, `labx_scalars`,
  `verify_types_scalars`, `spec_v1161_adversarial`, `spec_key_eq_symmetry`, `cov_containers_dunders`.

## Docs updated (lockstep)

`09-types.md` (NaN-key rejection), `10-stdlib.md` (astype throw, DataFrame index validation,
describe-empty, agg diagnostic), `12-exceptions.md` (new rows: astype / DataFrame index / agg / NaN-key;
Set-read unhashable), `13-course-03-strings.md` (`\xHH` code point), `30-bonus-05-tensors.md`
(round half-away-from-zero). Doc bundle regenerated; all 242 doc code-fences pass.

## Part-1 deferred-debt items (cleared before this round)

The ten prior-round debt items (serde name-guards, DEFLATE corruption classes, proc/time error tests,
docs↔impl message pinning, RNG cross-platform caveat, itertools negative-r throw, textwrap space-run
collapse, tensor int→size_t, `Complex.is_zero`→`iszero` rename, statistics/semver value pins,
`BytesIO.writelines`) shipped in commit `290757b`. A latent `-Wshadow -Werror` break the prior session
left in `test_serde.cpp` was fixed. `-fanalyzer` confirmed already absent from every build preset.
