# `tabular` module — doc-vs-impl verification

Test: `tools/tests/scripts/verify_tabular.ki` (+ `.expected` = `OK tabular\n`), 183 asserts.
Impl: `src/kirito/stdlib_kimodules.hpp` (`tabular` frozen-source module, lines ~1264-2241).
Docs: `docs/pages/10-stdlib.md` `## tabular` + `docs/pages/28-bonus-03-data.md`.
Runner: `build-debug/ki`. **No `src/` edits.**

## Discrepancies

### 1. `Series.all()` / `Series.any()` are documented but NOT implemented
`docs/pages/10-stdlib.md` (Series section) states a boolean Series has no single truth value and
must be reduced first via **`s.all()`/`s.any()`**. Neither method exists on the `Series` class in the
impl — both calls raise (`has no attribute`/name error). The only reductions on `Series` are
`sum/count/mean/min/max/median/variance/std/prod` (+ `unique/nunique/valuecounts`). Masking with the
mask directly (`df[df["c"] > v]`) works, so the workaround exists, but the documented `all`/`any`
entry points are missing. **Pinned** by asserting both raise. (Fix would be a 2-line addition in
`src/`; flagged, not applied.)

### 2. `DataFrame.rowat(pos)` returns a `Series`, docs say "one row as a Dict"
`docs/pages/10-stdlib.md` describes `rowat(pos)` as "(one row as a Dict)". The impl returns a
`Series(vals, columns, index[pos])` — a Series, not a Dict (`type(df.rowat(0)) == "Series"`). Both
support `row["col"]`, so the doc example `df.iloc[0]["name"]` works either way, but the stated type
is wrong. (Use `df.torows()` for Dict rows.) Doc-only fix. **Pinned.**

## Pinned current behaviour / edge notes (not bugs)

- **`head`/`tail` with negative `n`** — asymmetric, follows directly from the slice/offset math and is
  left as-is:
  - `Series.head(n)` = `values[0:n]`, so `head(-1)` drops the last element (`[1,2,3,4].head(-1) == [1,2,3]`).
  - `Series.tail(n)` computes `start = len - n`; `tail(-1)` → `start = len+1 > len` → **empty**.
  - `DataFrame.head(-1)` / `DataFrame.tail(-1)` both → an **empty** frame (0 rows).
- **CSV inference gating** (`_looksnumeric`): a cell is only inferred numeric in plain decimal form.
  A hex/oct/bin-looking cell (`0x1F`) or a whitespace-padded field stays a **String** — deliberate, so
  a hex-looking id isn't silently converted. `True`/`true`/`False`/`false` → Bool; empty cell → None.
- **`astype(unknown)`** falls back to `String` (the `conv` default) rather than throwing.
- **`Series.unique`/`nunique` keep `None`** as one distinct value; **`valuecounts` skips** missing
  (`None`/NaN). Documented, verified.
- **Duplicate column names** (via `columns=["a","a"]`): `df.columns` keeps both entries, but
  `df.data` is a Dict so only the **last** column survives (`df["a"]` returns the last). No error —
  silently lossy. Pinned; matches the "column order from a dict is not guaranteed" caveat spirit.
- **Ragged list-of-lists rows**: a later row **shorter** than the first row **throws** (index
  out-of-range on `r[ci]`); a later row **longer** is **silently truncated** to the first row's width.
  (List-of-**dicts** ragged input is fine — key union, missing → None.)
- **NaN-key handling (the recent fix), verified NOT to crash:**
  - `groupby` on a column with a Float **NaN** key **drops** those rows (a NaN is write-only in a
    Kirito Dict); **None** keys are **kept** as a valid group.
  - `merge` with a NaN join key does not crash: a NaN key is simply non-matching (dropped under
    `inner`, emitted as an unmatched row under `left`/`right`/`outer`). Confirmed inner + outer.
- **`DataFrame` has no arithmetic operators** (`df + 1` raises). Docs say arithmetic is "on `Series` —
  a `DataFrame` is operated on per-column", i.e. you select a column Series and operate on it; the
  frame itself is not an arithmetic operand. Consistent reading, pinned.
- **Frame aggregations** `sum/mean/min/max/std` cover **numeric** columns only (→ Series indexed by
  those columns); **`count`** is the exception, tallying non-null over **every** column. `groupby`
  numeric reductions likewise skip non-numeric value columns, while `groupby.count` reports each
  group's **row count** across every value column. All verified.
- **`sortvalues`** (Series + DataFrame): missing values sort to the **end** regardless of
  `ascending` (pandas `na_position='last'`). Verified both directions.

## Coverage

Series: construction (values/index/name, length-mismatch throw), label-vs-position `_getitem_`/`iat`,
`copy`; arithmetic `+ - * / // %` (scalar broadcast + Series-Series aligned, length-mismatch both
orders); comparisons `> >= < <= == !=` → boolean Series, `eq`/`ne`/`isin`; aggregations
`sum/count/mean/min/max/median/variance/std/prod` (skip missing, Bool as 0/1, all-missing → None /
0 / 1, n<2 variance); `unique`/`nunique`/`valuecounts`; `apply`/`map`/`astype`(Int/Float/Bool/String)/
`fillna`/`dropna`; `head`/`tail`(+defaults, negative n)/`sortvalues`(asc/desc, na-last)/`resetindex`.

DataFrame: construction from dict-of-columns (+`columns=`), list-of-lists (+default cols), list-of-
dicts (key union, missing None), empty/single-cell, ragged-throw; `readcsv` (header/infer, Float/Bool/
Integer/String inference, hex-gating, blank-line skip, short-row-None, `header=False`, `infer=False`,
over-wide-row throw); selection `df["c"]`/`df[["a","b"]]`/boolean-Series-mask/boolean-List-mask (+wrong
length throw); `iloc`/`loc` (scalar + list), `column`/`at`/`iat`/`rowat`; mutation `df["c"]=`
(Series/List/scalar, length throw)/`assign`/`rename`/`drop`/`copy`; frame aggregations `sum/mean/min/
max/count/apply/describe`; `sortvalues`; views `todict`/`torows`/`iterrows`/`slice`/`head`/`tail`/
`tocsv` round-trip; `setindex`/`resetindex`.

GroupBy: `mean/sum/min/max/size/count/agg`(max/median)/`apply`; NaN-key drop, None-key keep.

merge: inner/left/right/outer, `_x`/`_y` suffixing, DataFrame method form, NaN join key no-crash.
concat: column union + None fill. Hardening via `raises_msg`: bad merge `how` (both entry points),
missing column, unknown `agg` reducer, ragged/length errors.
