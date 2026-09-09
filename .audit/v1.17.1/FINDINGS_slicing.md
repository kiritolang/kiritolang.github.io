# v1.17.1 deep audit — multi-axis slicing feature (findings & fixes)

Fresh deep audit of the multi-axis basic-indexing / slicing feature (commit 91a6c10) — the newest,
least-audited surface since the v1.17.1 core audit. Three parallel subsystem auditors + orchestrator
probes, every finding REPRODUCED on a real binary (`build-bin/ki-{asan,release}`), marked CONFIRMED.
Per-auditor detail in `scan/{tensor_basicindex,slicing_lang,slice_collections_docs,slicing_orchestrator_probes}.md`.

## CONFIRMED findings — all FIXED this round (regression-tested, gate-verified)

- **S1 [HIGH] — Series slice-assign silently desynced values from index.**
  `stdlib_kimodules.hpp` `Series._setitem_`. `s[1:3] = [20,30,40,50]` grew `values` to len 7 while
  `index` stayed len 5 — the `len(values)==len(index)` invariant broken with NO error, corrupting all
  downstream label lookups/alignment. Reachable with ordinary values (no adversarial input).
  **Fix:** require the replacement length to equal the number of selected positions
  (`len(self.values[key])`) — matches pandas; the invariant now holds. Regression: `spec_slice_audit.ki`
  + exceptions-doc entry. Repro before fix: `values len 7, index len 5`; after: clean throw.

- **S2 [MED] — list extended-slice assignment: signed-overflow UB.** `runtime.hpp:592`, the
  `i += r.step` walk. `a[2:8:9223372036854775807] = [99]` tripped UBSan `signed integer overflow`; a
  matching-length RHS would then drive bogus (huge/negative) indices into `setElem`. **Fix:** compute
  the count via the overflow-safe `tns::rangeCount` and index as `start + j*step` (bounded within the
  clamped range) — SSOT with the tensor path.

- **S3 [MED] — tensor slice-assign: signed-overflow UB + silent drop.** `stdlib_tensor.hpp` `rangeCount`
  (`stop - start + step - 1`, and `-step` for INT64_MIN) and `indexGeometry` (`ax.step * ss`).
  `v[2:8:9223372036854775807] = 9.0` tripped UBSan; on release the wrong count silently dropped the
  assignment. **Fix:** `rangeCount` divides the *bounded span* by the step magnitude (unsigned negation,
  valid for INT64_MIN) → a huge step yields count 1; `indexGeometry` only forms `step*stride` when
  count > 1 (where it's provably bounded by numel), else 0 (unused). Fixes both the read and write
  paths (shared code). Regression: `spec_slice_audit.ki` (runs under asan/UBSan).

- **S4 [LOW] — DataFrame row-slice-assign error message absent from the exceptions reference.**
  `stdlib_kimodules.hpp:1928` message was undocumented. **Fix:** added to `docs/pages/12-exceptions.md`
  (alongside the new Series slice-assign message). Behavior itself was already correct.

## Verified CORRECT (probed adversarially, not merely read) — the pass is provably deep
- **Tensor gather/scatter** (auditor 1): int/slice/ellipsis/newaxis mixes on 0-D…3-D, negative-step
  reversal incl. a 1M-element `[::-1,::-1]` under asan, empty/clamped/0-size ranges, rank-cap,
  complex gather/scatter, scalar/block/reversed/newaxis scatter ordering, **no partial write before an
  error**, grad-tensor refusal — all correct, no asan/UBSan report, results matched numpy. Offsets are
  bounded by `numel ≤ 64M`, so real memory offsets can never wrap.
- **Language machinery** (auditor 2): `SliceVal::children()` roots all three bounds; `MakeSlice`,
  `.indices()`, `InstanceValue::slice` all keep fresh bounds rooted (soak-tested `KIRITO_GC_THRESHOLD=1`
  + non-interned big ints under asan); `.indices()` matches CPython exactly incl. INT64 extremes;
  single-axis backward-compat unchanged; Ellipsis not const-folded; parser edge cases all clean-error.
- **Collections & docs** (auditor 3): all List slice reads + assigns (grow/shrink/insert/delete/
  extended/neg-step/self-assign) match hand computation with no partial mutation on error; String
  immutability + UTF-8 slicing; Series/DataFrame read slices stay index-aligned; all 242 doc fences run.
- **Orchestrator:** Slice/Ellipsis are sealed from serde/arith/hash/len (clean rejections, no silent
  corruption); copy/deepcopy/equality sane; the parser `LBracket` rewrite caused no subscript regression.

## Language-design note (not a bug; candidate future feature)
A multi-axis subscript delivers each axis as a **separate positional arg** to a user class's
`_getitem_` (`m[1, 2:4]` → `_getitem_(self, a, b)`), and there is no varargs `_getitem_(self, *keys)`,
so a user class cannot uniformly accept a variable number of axes. Internally consistent and documented
in code; flagged per the brief's "language-design gaps". A `*keys` protocol would be its own scoped round.
