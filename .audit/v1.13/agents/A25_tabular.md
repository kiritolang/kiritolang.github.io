# A25 — tabular audit (Series / DataFrame data-analysis library)

Source: `src/kirito/stdlib_kimodules.hpp` lines 1230–2199 (the `tabular` frozen-source Kirito module).
Tests: `tools/tests/scripts/tabular_*`, `audit_tabular.ki`, `examples/tabular_*.ki`, `examples/data/iris.csv`.
Method: static reasoning over Kirito source semantics + native ops. READ-ONLY; no build/run.

Status: COMPLETE.

## Surface enumerated
- **Series**: ctor(values, index, name), `_len_`/`_iter_`/`_getitem_`/`_setitem_` (label-first then position), iat, tolist, copy, `_binop` + `_add_/_sub_/_mul_/_div_/_floordiv_/_mod_`, `_gt_/_ge_/_lt_/_le_/_eq_/_ne_` + eq/ne, isin, sum/count/mean/min/max/median/variance/std/prod, unique/nunique/valuecounts, apply/map, astype, fillna/dropna, head/tail, sortvalues, resetindex, `_str_`.
- **DataFrame**: ctor(data=Dict|List-of-rows|List-of-lists, columns, index) + `_fromcolumns`/`_fromrows`, nrows/shape/`_len_`, rowat/rowsat, `_getitem_` (col String / col-subset List / boolean mask via Series or List) / `_setitem_`, `_mask`/`_subset`, column, at/iat, head/tail/slice, copy/`_datacopy`, rename/drop/assign, resetindex/setindex, `_aggcols`+sum/mean/min/max/std/count, describe, sortvalues, apply, iterrows/todict/torows, dropna/fillna, groupby, merge, tocsv, `_str_`. `iloc`/`loc` via `_Iloc`/`_Loc`.
- **GroupBy**: ctor, `_valuecols`, `_reduce`, sum/mean/min/max/std/count/size, agg(dict), apply.
- **Module funcs**: `_defaultcols`, `_merge`/merge, concat, readcsv; helpers `_isnan`/`_numeric`/`_isnumericcol`/`_range`/`_looksnumeric`/`_infer`.

Existing tests are extensive (audit_/deep_/r7_/r8_/r9_/spec_/probe_tabular.ki + unit test_tabular.cpp) and already pin many edge behaviours (duplicate-column corruption, astype fallback, head/tail negative-n, NaN-in-unique, None group/merge keys, ragged-column throw, na_position=last).

---
## Findings

### A25-1: groupby on a column containing Float NaN crashes (write-only NaN key)
- **severity**: High
- **location**: `stdlib_kimodules.hpp` `class GroupBy._init_` (lines ~1962-1970); root cause `collections.hpp probeBucket` (NaN is insertable but never findable — no identity short-circuit before `==`, NaN != NaN).
- **category**: bug / NaN-handling (links A06-7 write-only-NaN; A09-2/A15 tensor NaN-ordering family)
- **description**: `_init_` does `if k not in self.groups: self.keys.append(k); self.groups[k] = []` then `self.groups[k].append(i)`. For a Float NaN key, `k not in self.groups` is ALWAYS true (NaN never `==` itself), so it inserts a fresh write-only NaN entry, then immediately does a getitem `self.groups[k]` which fails to find the just-inserted key → a KeyError-style throw mid-construction. `None` keys work fine (`None==None`), which is why every existing test uses None and passes; **NaN is untested and crashes**.
- **failure-scenario**: `pd.DataFrame({"k":[1.0, math.nan, 2.0], "v":[1,2,3]}).groupby("k")` throws. Reachable straight from CSV: a cell literally `nan`/`inf` is coerced to Float NaN by `_infer` (see A25-4), so `readcsv(...).groupby("keycol")` crashes when the key column has a `nan` cell — a realistic missing-key case pandas silently drops.
- **proposed-test**: assert `throws(Function(): return pd.DataFrame({"k":[1.0, NAN, 2.0],"v":[1,2,3]}).groupby("k"))` to pin current behaviour, then (after fix) assert NaN rows form their own group or are dropped like pandas.
- **proposed-fix**: in `GroupBy._init_`, treat `_isnan(k)` keys explicitly — either skip them (pandas `dropna=True` default) or bucket them under a single canonical sentinel; do not use the raw NaN as a Dict key.
- **confidence**: High (write-only-NaN semantics confirmed in `probeBucket`; Float("nan") parse confirmed).

### A25-2: merge on a NaN join key crashes on the right frame, silently drops it on the left
- **severity**: Medium
- **location**: `stdlib_kimodules.hpp` `_merge` (lines ~2061-2068, rightidx build); same root cause as A25-1.
- **category**: bug / NaN-handling
- **description**: `_merge` builds `rightidx[k]` with `if k not in rightidx: rightidx[k] = []` then `rightidx[k].append(rpos)` — identical write-only-NaN crash when the RIGHT frame's `on` column holds a Float NaN. On the LEFT side, `if k in rightidx` for a NaN key is always false, so NaN-keyed left rows are silently dropped (inner) or None-filled (left/outer) rather than matched — asymmetric and divergent from any consistent rule. Existing tests only exercise `None` keys (r8_tabular), which work.
- **failure-scenario**: `pd.merge(L, pd.DataFrame({"id":[math.nan],"y":[1]}), on="id")` throws; with NaN only on the left it runs but drops the row.
- **proposed-test**: pin both: right-NaN throws; left-NaN dropped/None. After fix, define consistent NaN-key semantics (NaN never matches, like SQL NULL).
- **proposed-fix**: guard `_isnan(k)` when building `rightidx` and when probing from the left (a NaN key should match nothing, never be used as a Dict key).
- **confidence**: High.

### A25-3: duplicate column names on direct construction silently collapse (data loss)
- **severity**: Medium (KNOWN — pinned as "BUG" in deep_tabular.ki:83-92)
- **location**: `_fromrows`/`_fromcolumns`; backing store is `self.data` (a Dict keyed by column name).
- **category**: bug / duplicate-labels
- **description**: `DataFrame([[1,2],[3,4]], columns=["a","a"])` yields `.columns == ["a","a"]` but `self.data["a"]` holds only the SECOND column; the first is lost. `nrows()` reads the first slot so shape looks fine while `df["a"]` returns the wrong column. `readcsv` with a duplicated header instead throws a length-mismatch (deep_tabular:90) — inconsistent handling of the same condition. pandas de-duplicates headers (`a`, `a.1`).
- **failure-scenario**: any frame/CSV with repeated column names.
- **proposed-test**: already present (deep_tabular). Gap: no de-dup path exists.
- **proposed-fix**: reject duplicate column names in ctor with a clear error, or auto-suffix like pandas.
- **confidence**: High (confirmed + already tested).

### A25-4: _infer silently coerces text "nan"/"inf"/"infinity" cells to Float NaN/inf
- **severity**: Low
- **location**: `_infer`/`_looksnumeric` (lines ~1290-1320); `Float()` parses "nan"/"inf" (runtime.hpp:2895 "decimal 'inf'/'nan' still parse").
- **category**: bug / type-inference
- **description**: `_looksnumeric("nan")` is True (no 0x/0o/0b prefix, no surrounding space), so `_infer` calls `Float("nan")` → NaN and `Float("inf")` → inf. A CSV cell whose literal text is the word `nan`/`inf` (e.g. a category label, a gene name "INF") becomes a numeric special. This also feeds A25-1/A25-2 (a `nan` key cell crashes groupby/merge). pandas does the same NaN coercion by default, so it is defensible, but combined with the write-only-NaN crash it is a live hazard.
- **failure-scenario**: `readcsv("k\nnan\nfoo").groupby(...)` / any downstream `==`/hash on the NaN cell behaves as missing.
- **proposed-test**: assert `type(pd.readcsv("x\nnan")["x"][0]) == "Float"` and that it is NaN; document.
- **proposed-fix**: none required if intended; otherwise gate `nan`/`inf` inference behind an option.
- **confidence**: High.

### A25-5: head/tail negative-n behaviour inconsistent between Series and DataFrame
- **severity**: Low
- **location**: `Series.head/tail` (slice-based, lines ~1527-1533) vs `DataFrame.head/tail` (range-based, ~1741-1753).
- **category**: bug / off-by-one / API-consistency
- **description**: `Series.head(-1)` uses `values[0:-1]` → drops the last element (slice semantics, tested deep_tabular). `DataFrame.head(-1)` computes `_range(n if n<nrows else nrows)` = `_range(-1)` → empty frame. So the same call means "all but last" on a Series but "empty" on a DataFrame. Neither matches pandas' `head(-n)` = all-but-last-n for the frame. `tail` similarly diverges.
- **failure-scenario**: `df.head(-1)` returns 0 rows unexpectedly.
- **proposed-test**: assert `pd.DataFrame({"a":[1,2,3]}).head(-1).nrows()` and pin.
- **proposed-fix**: make DataFrame head/tail use the same slice semantics as Series (or clamp both consistently).
- **confidence**: High.

### A25-6: coverage-gap — NaN in a groupby/merge KEY column is never tested
- **severity**: coverage-gap
- **location**: tests use only `None` keys (r8_tabular:77-88); no test puts `math.nan` in a group-by or merge `on` column.
- **category**: coverage-gap
- **description**: The whole test corpus carefully validates None-as-key (works) but never NaN-as-key (crashes, A25-1/2). This gap hid the crash.
- **proposed-test**: add NaN-key cases for `groupby` and both merge sides.
- **confidence**: High.

### A25-7: DataFrame from ragged List-of-Lists rows crashes with a raw IndexError
- **severity**: Low
- **location**: `_fromrows` non-dict branch (lines ~1646-1656): `ncols = len(rows[0])`, then `r[ci]` for every row.
- **category**: error-handling / robustness
- **description**: If a later row is SHORTER than `rows[0]`, `r[ci]` throws a generic index error (not a friendly "row N has wrong width"). If LONGER, surplus cells are silently dropped. Only the Dict-of-columns path validates equal lengths; the row-list path does not.
- **failure-scenario**: `pd.DataFrame([[1,2],[3]])` → unhelpful index error.
- **proposed-test**: assert a clear error message for ragged row lists.
- **proposed-fix**: validate every row length against `ncols` in `_fromrows`.
- **confidence**: High.

### A25-8: GroupBy.agg / column reductions throw raw KeyErrors on bad spec
- **severity**: Low
- **location**: `GroupBy.agg` (lines ~2021-2038): `reducers[spec[c]]` and `self.frame.data[c]`.
- **category**: error-handling
- **description**: An unknown reducer name (e.g. `{"v":"var"}`) → `reducers["var"]` raw KeyError; a spec column absent from the frame → `self.frame.data[c]` raw KeyError. No actionable message naming the offending reducer/column. Also `agg` applies a reducer to non-numeric columns with silent results (`sum`→0, `median`→None) unlike the numeric-only `_reduce`.
- **failure-scenario**: `g.agg({"v":"variance"})` → cryptic KeyError (note "variance" is a Series method but the agg table only has "std").
- **proposed-test**: assert a clear error for an unknown reducer and a missing column.
- **proposed-fix**: validate `spec` keys against columns and reducer names against the table with named errors.
- **confidence**: High.

### A25-9: Series/DataFrame binary ops and column assignment align by POSITION, not index label
- **severity**: Low (documented divergence)
- **location**: `Series._binop` (lines ~1359-1371); `DataFrame._setitem_` with a Series value (~1699-1700).
- **category**: bug / silently-wrong-results (vs pandas expectation)
- **description**: `s1 + s2` and `df["c"] = someSeries` align purely by position and ignore the index labels (only lengths are checked). Users coming from pandas expect label alignment; a same-length but differently-ordered Series produces silently wrong results. The `_binop` header comment documents "aligned by position", so this is intentional, but it is a real foot-gun and a boolean mask from a differently-indexed Series is likewise applied by position.
- **failure-scenario**: two Series with the same labels in different order add the wrong pairs.
- **proposed-test**: pin the position-alignment behaviour explicitly so a future label-alignment change is a conscious break.
- **confidence**: High.

### A25-10: tocsv → readcsv round-trip converts empty-string cells to None (data loss)
- **severity**: Low
- **location**: `DataFrame.tocsv` (lines ~1913-1923) writes `"" if v==None else String(v)`; `_infer("")` returns None.
- **category**: bug / round-trip fidelity
- **description**: Both `None` and the empty String `""` serialize to an empty CSV field, and `readcsv` infers an empty field back to `None`. So a genuine empty-string cell becomes None after a round-trip. (pandas has the same ambiguity by default, so low severity, but worth pinning.)
- **failure-scenario**: `readcsv(df.tocsv())` where `df` has `""` cells.
- **proposed-test**: assert the round-trip turns `""` into None.
- **confidence**: High.

### A25-11: DRY — repeated per-column clone loop and recomputed _numeric
- **severity**: dry
- **location**: `copy`, `_datacopy`, `resetindex`, `setindex`, `rename`, `_subset`, `_fromcolumns` all repeat `newdata[c] = List(self.data[c])`; `Series.std`→`variance`→`mean`→`sum` each recompute `_numeric(self.values)` (3-4 passes for one std).
- **category**: dry / minor-perf
- **description**: The column-copy idiom is duplicated in 6+ places; a single private `_clonecols([cols])` would DRY it. The aggregation chain recomputes the numeric-filtered list several times per call; caching it once would halve/third the work for `std`/`variance` on large columns.
- **proposed-fix**: extract a column-clone helper; compute `_numeric` once and pass it down the sum/mean/variance/std chain.
- **confidence**: High.

### A25-12: describe on an all-non-numeric frame yields a degenerate frame (no error)
- **severity**: Low
- **location**: `DataFrame.describe` (lines ~1830-1840).
- **category**: bug / edge (empty)
- **description**: With no numeric columns, `numcols=[]`, `newdata={}`, so `DataFrame({}, [], stats)` is built with an index of length 6 (the stat names) but `nrows()==0` (no columns) — an internally inconsistent frame (index length ≠ row count). It doesn't crash, but printing/iterating it is misleading. pandas returns a describe over the object columns (count/unique/top/freq) instead.
- **failure-scenario**: `pd.DataFrame({"s":["a","b"]}).describe()`.
- **proposed-test**: pin the empty/degenerate result.
- **proposed-fix**: return an empty frame with a consistent index, or raise/note when no numeric columns exist.
- **confidence**: Medium (degenerate-frame shape reasoned statically).

### A25-13: unbounded cartesian blowup on many-to-many merge
- **severity**: Low
- **location**: `_merge` (lines ~2107-2116) — duplicate keys on both sides emit M×N rows.
- **category**: weak-spot / resource
- **description**: A join key duplicated M times on the left and N on the right emits M×N output rows with no guard, so an adversarial or accidental many-to-many key can OOM. pandas has the same semantics but warns; here there is no guard and no `validate=` option.
- **failure-scenario**: two frames each with 10k rows all sharing one key → 100M output rows.
- **proposed-test**: a bounded many-to-many merge count check (not a huge one).
- **proposed-fix**: optionally cap/validate join multiplicity (out of scope, note only).
- **confidence**: High.

### A25-14: coverage-gap — several edges untested
- **severity**: coverage-gap
- **location**: various.
- **category**: coverage-gap
- **description**: Gaps observed: (a) NaN group/merge keys (A25-6); (b) ragged List-of-Lists rows (A25-7) — only Dict-of-columns ragged is tested; (c) `agg` with an unknown reducer name / missing column (A25-8); (d) `describe` with zero numeric columns (A25-12); (e) DataFrame `head/tail` with negative n (A25-5) — only Series negative-n is tested; (f) empty DataFrame (`DataFrame()`) then `_setitem_` first column syncing the index; (g) `_getitem_` mask where a List's first element is Bool but later elements are not pure Bool (only `key[0]` is type-checked at line ~1692); (h) `astype("Integer")` of a None/String element (throws); (i) boolean mask supplied as a Series with a mismatched index (position-aligned, A25-9).
- **proposed-test**: add focused cases for each.
- **confidence**: High.

### A25-15: Series min/max are numeric-only (diverge from pandas object min/max)
- **severity**: Low
- **location**: `Series.min/max` (lines ~1431-1436) use `_numeric` then `_min`/`_max`.
- **category**: bug / API divergence
- **description**: On a String (or mixed non-numeric) Series, `.min()`/`.max()` return `None` because `_numeric` drops all non-numeric values, whereas pandas returns the lexicographic min/max string. `median`/`sum`/`mean` are numeric-only by design, but `min`/`max` are well-defined on any orderable type and users will expect them to work on strings.
- **failure-scenario**: `pd.Series(["b","a","c"]).min()` returns None instead of "a".
- **proposed-test**: pin current (numeric-only) behaviour or extend to orderable types.
- **proposed-fix**: for min/max, fall back to ordering the non-missing values of any single comparable dtype.
- **confidence**: High.
