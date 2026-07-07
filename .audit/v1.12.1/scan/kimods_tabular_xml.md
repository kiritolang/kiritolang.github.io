# v1.12.1 (audit loop) — tabular + xml modules (frozen-source Kirito in stdlib_kimodules.hpp)

Subsystem: tabular (pandas-like) + xml (ElementTree-like) modules.
Source: src/kirito/stdlib_kimodules.hpp (tabular ~line 1268, xml ~line 2261)
Probe binary: ./build-debug/ki

## LOG
- Created notebook. Starting to read source.

## FINDINGS

### F1 [MED] DataFrame ctor accepts index of wrong length silently (no validation) -> delayed crash
- where: src/kirito/stdlib_kimodules.hpp:1648-1658 (`DataFrame._init_`; `self.index = _range(nrows) if index==None else List(index)` — never length-checked)
- repro:
  ```
  var tb = import("tabular")
  var df = tb.DataFrame({"a":[1,2,3]}, None, [10, 20])   # 3 rows, 2 index labels
  df.index      # => [10, 20]  (accepted silently)
  df.rowat(2)   # THROW: index out of range  (delayed)
  String(df)    # THROW: index out of range
  ```
- actual: construction succeeds with an index shorter/longer than the row count; later row/label ops crash with a generic "index out of range". expected: pandas raises "Length mismatch: Expected N rows, got M" at construction.
- also manifests in `describe()` when there are 0 numeric columns: returns a frame with index=['count','mean','std','min','median','max'] (6 labels) but shape [0,0] — inconsistent object.
- fix idea: after building columns, if index != None assert len(index) == nrows() else throw.

### F2 [MED] DataFrame from list-of-lists: ragged rows -> silent truncation OR ungraceful crash
- where: src/kirito/stdlib_kimodules.hpp:1698-1708 (`_fromrows` else-branch: `ncols = len(rows[0])`, then `r[ci]` for every row)
- repro:
  ```
  var tb = import("tabular")
  tb.DataFrame([[1,2],[3,4,5]]).todict()   # => {'col0':[1,3],'col1':[2,4]}  (the 5 is silently dropped)
  tb.DataFrame([[1,2,3],[4,5]]).todict()   # THROW: index out of range      (short row)
  ```
- actual: a row LONGER than row[0] silently loses its surplus cells; a row SHORTER crashes with "index out of range". expected: consistent behavior — either reject ragged rows with a clear message, or pad short rows / error on long rows (as readcsv does).
- fix idea: validate every row length against ncols (like readcsv does for CSV) and throw a clear message, or pad with None.

### F3 [LOW] readcsv/_infer: text cells "nan"/"inf"/"Infinity" become Float NaN/inf (indistinguishable from missing)
- where: src/kirito/stdlib_kimodules.hpp:1329-1359 (`_looksnumeric` only rejects whitespace/base-prefix; `_infer` then does Float(text) which accepts "nan"/"inf")
- repro:
  ```
  var tb = import("tabular")
  tb.readcsv("h\nnan")["h"].values[0]   # => Float NaN  (then counted as MISSING by _isnan)
  tb.readcsv("h\ninf")["h"].values[0]   # => Float inf
  ```
- actual: a literal text cell "nan" is inferred as Float NaN and thereafter treated as a missing value by every aggregation/dropna; "inf" becomes an infinite float. expected: debatable (pandas also coerces "nan"->NaN), but a data cell that reads "nan" silently becoming missing is a data-integrity surprise; note as SUSPECT/LOW.
- fix idea: if a stricter numeric-cell check is desired, require at least one digit in _looksnumeric so pure "nan"/"inf" stay String.

### F4 [LOW/SUSPECT] groupby.agg 'sum'/'mean' on a non-numeric column silently returns 0/None (no error, no concat)
- where: src/kirito/stdlib_kimodules.hpp:2077-2094 (`agg` does not check `_isnumericcol`; Series.sum() over `_numeric` drops non-numeric -> 0)
- repro:
  ```
  var tb = import("tabular")
  var df = tb.DataFrame({"g":["a","a"], "s":["x","y"]})
  df.groupby("g").agg({"s":"sum"}).todict()   # => {'s':[0]}   (pandas concatenates -> "xy")
  ```
- actual: summing a string column via agg yields 0 (mean yields None) silently. expected: raise, or (pandas) concatenate strings. Minor.
- fix idea: intentional? If so document; else guard non-numeric in agg sum/mean.

### F5 [MED] head/tail with negative n: Series vs DataFrame disagree; DataFrame ignores pandas' drop-from-end semantics
- where: src/kirito/stdlib_kimodules.hpp:1579-1585 (Series.head/tail slice-based) vs 1793-1805 (DataFrame.head/tail via `_range`)
- repro:
  ```
  var tb = import("tabular")
  var df = tb.DataFrame({"a":[1,2,3,4,5]})
  df.head(-2).nrows()                       # => 0
  tb.Series([1,2,3,4,5]).head(-2).values    # => [1,2,3]   (all but last 2)
  ```
- actual: Series.head(-2) returns all-but-last-2 (pandas semantics, via `values[0:n]`); DataFrame.head(-2) returns EMPTY (because `_range(-2)` yields []). tail(-2) is empty on both but pandas would drop the first 2. expected: consistent negative-n handling across Series and DataFrame.
- fix idea: implement DataFrame.head/tail via the same slice/offset math the Series versions use (compute effective count = nrows + n when n < 0).
