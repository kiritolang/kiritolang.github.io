# Slice / collections / tabular / docs audit — v1.17.1

Scope: first-class `Slice` value on built-in collections + tabular module + slice assignment +
docs-vs-reality. Reproduced on `build-bin/ki-release` and `build-bin/ki-asan`
(`ASAN_OPTIONS=detect_leaks=0`, UBSan enabled). No source or tests edited.

Two slice-resolution implementations exist and diverge on overflow safety:
- READ path `sliceIndices` (src/kirito/native.hpp:54) — **count-driven**, explicitly overflow-safe.
- ASSIGN paths use `resolveSlice` (src/kirito/stdlib_tensor.hpp:775) then iterate/count with a
  naive `i += step` / `stop - start + step - 1`, which is **not** overflow-safe. All overflow
  findings below live on assign paths for this reason.

---

## FINDINGS

### F1 — Series slice-assign silently desyncs values from index (HIGH, CONFIRMED)
- **Where:** src/kirito/stdlib_kimodules.hpp:1501-1506 (`Series._setitem_`, Slice branch).
- **Reproducer:**
  ```
  var s4 = tb.Series([1,2,3,4,5])
  s4[1:3] = [20,30,40,50]      # step==1 splice GROWS values by +2
  # values=[1,20,30,40,50,4,5]  (len 7)   index=[0,1,2,3,4]  (len 5)
  ```
- **Actual vs expected:** `len(s4.values)==7` but `len(s4.index)==5`. The class invariant that
  `_init_` enforces (`len(values)==len(index)`, else it throws
  "Series: index length does not match values length") is silently broken. pandas raises on a
  length-changing slice-assign; Kirito silently corrupts. Downstream label lookups / row alignment
  are now wrong (`s4[3]` returns the shifted value 40, not the original index-3 element).
- **Root cause:** the Slice branch does `self.values[key] = value`, which delegates to the List
  step==1 splice that resizes, but never adjusts `self.index`. Extended-step assigns (`s[::2]=…`)
  can't resize, so those stay aligned; only the contiguous case corrupts.
- **Note:** reachable with ordinary values (no adversarial input) — this is the most practically
  dangerous finding.

### F2 — List extended-slice assign: signed-overflow UB on huge step (MEDIUM, CONFIRMED)
- **Where:** src/kirito/runtime.hpp:592 (`ListVal::setItem`, extended-slice branch, `i += r.step`).
- **Reproducer (asan/ubsan):**
  ```
  var a = [1,2,3,4,5,6,7,8]
  a[2:8:9223372036854775807] = [99]
  ```
  UBSan: `runtime.hpp:592: signed integer overflow: 2 + 9223372036854775807 cannot be represented`.
- **Actual vs expected:** the loop pushes index 2 then does `i += step`, which overflows (UB). On
  ki-release the wrap produces bogus indices (a huge negative and 0) so the size check reports
  "3 target(s) but 1 value(s)" instead of the correct 1 target — and the collected index list holds
  a wrapped negative index that, had the RHS length matched, would be cast to `size_t` and drive an
  **out-of-bounds `setElem` write** (silent memory corruption). Here it happens to be caught by the
  size-mismatch check, but the behavior is UB-dependent, not by-design.
- **Root cause:** assign path uses a naive `i += step` walk instead of the count-driven approach the
  READ path (`sliceIndices`) uses precisely to avoid this overflow.
- **Note:** requires an adversarial near-`INT64_MAX` step; step in [-INT64_MAX .. small] cases do not
  overflow (verified `9223372036854775807`, `-9223372036854775807`, `INT64_MIN` with start==0/end
  boundaries pass cleanly — overflow only when `start + step` exceeds `INT64_MAX`).

### F3 — Tensor slice-assign: signed-overflow UB on huge step (MEDIUM, CONFIRMED)
- **Where:** src/kirito/stdlib_tensor.hpp:805 (`rangeCount`, `stop - start + step - 1`).
- **Reproducer (asan/ubsan):**
  ```
  var v = T.arange(0, 8, 1)
  v[2:8:9223372036854775807] = 9.0
  ```
  UBSan: `stdlib_tensor.hpp:805: signed integer overflow: 9223372036854775807 + 6 …`.
- **Actual vs expected:** should set index 2 to 9.0. On ki-release the overflow yields a wrong
  (zero) element count, so the assignment is **silently dropped** — tensor unchanged
  (`[0..7]`), no error. The READ path (`v[2:8:MAX]`) does NOT hit this (prints `[2.0]` cleanly),
  so the bug is specific to the assign/scatter geometry.
- **Root cause:** same class as F2 — assign geometry counts via non-overflow-safe arithmetic.
- **Note:** adversarial step required; normal steps unaffected.

### F4 — DataFrame row-slice-assign error message missing from exceptions doc (LOW, CONFIRMED)
- **Where:** message at src/kirito/stdlib_kimodules.hpp:1918; docs/pages/12-exceptions.md (tabular
  table ~843-853) does not list it.
- **Detail:** `df[a:b] = …` throws
  `"DataFrame: row slice-assignment (df[a:b] = ...) is not supported; assign a column df[name] = ...
  or use a boolean mask"`. This is a message tied to the new first-class Slice feature and is not
  in the exceptions reference (nor anywhere else under docs/). Doc-completeness gap only; the
  behavior itself is correct and clean.

---

## Verified CORRECT

List slice READ (src/kirito/runtime.hpp:1733, via native.hpp:54):
- `[10,20,30,40][1:3]`→[20,30]; `[-3:-1]`→[20,30]; `[1:100]`→[20,30,40] (clamp); `[3:1]`→[]
- `[::-1]`→[40,30,20,10]; `[::2]`→[10,30]; `[::-2]`→[40,20]
- `xs[slice(1,4)]` == `xs[1:4]` == [20,30,40]; `[::0]` throws "slice step cannot be zero"
- READ path overflow-safe: huge/`INT64_MIN` steps produce correct 1-element results, no UBSan hit.

List slice ASSIGN (src/kirito/runtime.hpp:571):
- grow `a[1:3]=[20,30,40]`→[1,20,30,40,4,5]; shrink `b[1:4]=[9]`→[1,9,5];
  insert `c[1:1]=[99]`→[1,99,2,3]; delete `d[1:3]=[]`→[1,4]; extended `e[::2]=[10,20,30]`→[10,2,20,4,30,6]
- negative-step assign `k[::-1]=[10..50]`→[50,40,30,20,10]; self-assign `h[:]=h`→[1,2,3]
- **NO partial mutation on error:** extended size mismatch (`f[::2]=[10,20]`) throws
  "list slice assignment size mismatch: 3 target(s) but 2 value(s)" and leaves `f` fully unchanged
  (the size check precedes the mutation loop). Non-iterable RHS (`g[0:2]=5`) throws
  "type 'Integer' is not iterable", `g` unchanged.

String slice immutability (src/kirito/runtime.hpp:1180):
- `"abc"[0:1]="x"` throws "type 'String' does not support item assignment"; UTF-8 slice correct
  (`"café"[3]`→é, `"café"[::-1]`→éfac, len 4).

Bytes slice: `Bytes([...])[0:1]`→b'H', `[::-1]`, `[::2]` correct.

Series (src/kirito/stdlib_kimodules.hpp:1495):
- `s[1:4]` keeps index aligned ([20,30,40]/[1,2,3]); `s[::-1]` and `s[::2]` align index; custom
  string index sliced in lockstep with values; same-length slice-assign preserves alignment;
  empty Series slice → empty values+index; out-of-range slice → empty (no throw). (Only the
  resizing slice-assign in F1 corrupts.)

DataFrame (src/kirito/stdlib_kimodules.hpp:1902):
- `df[1:3]`, `df[::-1]`, `df[2:3]`, `df[10:20]` (empty), `df[::2]`, and custom-index frames all
  preserve columns and slice the index in lockstep with every column. `df[a:b]=…` throws cleanly
  with no mutation.

`slice()` builtin / docs (docs/pages/09-types.md, 30-bonus-05-tensors.md, 08-builtins.md):
- `slice(1,8,2)` → .start/.stop/.step = 1/8/2; `slice(None,5,None).indices(3)`→[0,3,1];
  `slice(None,None,-1).indices(4)`→[3,-1,-1]; `slice(1,4).indices(10)`→[1,4,1];
  `slice(1,2,0).indices(5)` throws "slice step cannot be zero". `type(...)`→"Ellipsis".
- Tensor multi-axis doc examples verified against the binary exactly:
  `m[:,2:4]`, `m[1,:]`, `m[:,1]`, `m[...,0]`, `m[:,None].shape()`→[3,1,4], `m[:,::-1]`.
- `python3 tools/scripts/test_docs_examples.py --ki build-bin/ki-release`: 282 blocks,
  242 ran, 40 skipped, **0 failed**. (Caveat: this harness only asserts examples RUN without error;
  it does NOT compare printed output to the `#` comments — output correctness above was hand-verified.)
- Exceptions doc contains: "slice step cannot be zero" (265), "list slice assignment size mismatch"
  (266), "type '<T>' does not support item assignment" (236), "slice indices must be Integer or None"
  (264). Only the DataFrame row-slice-assign message (F4) is missing.

Nulls: empty Series/DataFrame slices return empty (not None/garbage); None cell values pass through
slices unchanged as ordinary handles. No null-specific slice defect found.
