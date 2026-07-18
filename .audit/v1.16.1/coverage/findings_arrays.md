# Coverage findings — numeric-array stdlib (tabular / matrix / tensor)

Test: `tests/scripts/cov_arrays.ki` (296 assertions), pinned against `build-bin/ki-release`.
Every assertion reflects OBSERVED behavior; where the impl diverges from `docs/pages/10-stdlib.md`
the test pins the real behavior and the delta is listed here. No `src/` or `docs/` was modified.

## Doc deltas / silent-error observations

### D1 (SILENT ERROR / doc contradiction) — a `Series` is always truthy in `if`/`while`
`docs/pages/10-stdlib.md` (Series section, ~L1508): "Because `==`/`!=` are element-wise, a Series
has no single truth value — using one directly in an `if`/`while` is an error; reduce it first
(`s.all()`/`s.any()`) or index with the mask."

Actual: using a Series in a boolean context does NOT throw. A Series is unconditionally truthy —
even an all-`False` boolean Series and an EMPTY Series both take the `if` branch.

Repro:
```
var tb = import("tabular")
var m = tb.Series([1, 2]) == tb.Series([1, 3])   # boolean Series [True, False]
if m:                                            # docs say this should error
    io.print("reached")                          # -> prints "reached"
if tb.Series([False, False]):                    # all-False -> still truthy
    io.print("still truthy")                     # -> prints
if tb.Series([]):                                # empty -> still truthy
    io.print("empty truthy")                     # -> prints
# Bool(tb.Series([False])) == True
```
Impact: a mask accidentally used where a scalar bool is expected is silently accepted as `True`
instead of raising — the exact footgun the docs claim is guarded. Either the docs overstate the guard
or the `truthy` slot for Series is missing. Test pins the real behavior (`not threw(...)`, truthy).

### D2 (message wording) — `tensor.reciprocal(0)` raises a division-by-zero message, not "math domain error"
`docs/pages/10-stdlib.md` (~L1745) lists `reciprocal` of `0` among the cases that "throw a clear
`tensor ... : math domain error`". Actual message:
```
tensor division by zero (reciprocal of 0)
```
It still throws (no silent NaN/inf), so the safety contract holds — only the message family differs
from the documented "math domain error" wording. Test asserts the real substring `reciprocal of 0`.

## Behavior notes (documented-but-worth-pinning; not bugs)

- Empty-Series reductions differ from tensor's. `tabular` returns identities/`None` rather than
  throwing: `Series([]).sum() == 0`, `.prod() == 1`, `.count() == 0`, and `.mean()/.min()/.max()/
  .std()/.median()` all return `None` (no exception). Contrast the tensor contract, where
  `mean/min/max/std/var/ptp/median` on an empty axis/tensor THROW (`sum`/`prod` return identities).
  Both are internally consistent; the two modules just chose different empty-reduction policies.
- F14-4 ragged-row contract holds exactly. `DataFrame([[1,2],[3,4,5]], columns=["a","b"])` ->
  `DataFrame: row 1 has 3 fields but the frame is 2 wide`; the too-short case ->
  `... row 1 has 1 fields but the frame is 2 wide`. No silent truncation of a long row, no bare index
  error on a short one.
- `readcsv` wide-row guard -> `readcsv: row 2 has 3 fields, expected 2` (different wording from the
  DataFrame constructor's, but equally explicit); short rows pad trailing cells with `None`; a
  multi-column empty field (`a,,c`) survives the `tocsv`->`readcsv` round-trip.
- groupby group order is first-seen (not sorted): the first key encountered in the frame leads.
  `groupby(col).count()` spans the value columns (excludes the group-key column) and reports each
  group's row count per value column; `std` of a single-element group is `None`.
- merge collision disambiguates a shared non-key column to `<name>_x` / `<name>_y` (pandas-style).
- matrix asymmetry (documented, verified): `inverse()` on a singular matrix throws
  `matrix is singular (no inverse)` (contains "singular"); `determinant()` of the same returns `0.0`.
  `2 * A` (scalar on the left) throws — scalar must be the right operand.
- tensor autograd: Complex + `requiresgrad=True` throws
  `gradients are Float-only (a Complex tensor cannot require grad)`; `zerograd()` resets `.grad` to
  `None`; `detach().requiresgrad() == False`.
- einsum rejects a repeated output label: `einsum: output label 'i' appears more than once`.

## Verdict
No functional bugs found beyond D1 (the Series-truthiness guard the docs promise but the impl does not
enforce). D2 is a cosmetic message-family mismatch (the guard itself fires). Everything else matches
the documented contracts.
