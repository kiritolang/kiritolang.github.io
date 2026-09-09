# Audit scan — Tensor basic (numpy-style multi-axis) indexing

Scope: gather/scatter for `t[int, slice, ..., None]` in `src/kirito/stdlib_tensor.hpp`
— `IndexAxis`, `rangeCount`, `IndexGeom`, `indexGeometry`, `basicIndex<T>`,
`buildIndexPlan`, `TensorVal::getItem`/`setItem` (basicIdx branches), and
`tns::resolveSlice`/`SliceRange`. Engine caps in `src/kirito/tensor.hpp`
(`kMaxElems = 64*1024*1024`, `kMaxRank = 64`, `checkedNumel`, `rowMajorStrides`).

Method: black-box adversarial probing against the preserved sanitizer binary
`build-bin/ki-asan` (`-fsanitize=address,undefined -fno-sanitize-recover=all`;
UBSan handlers confirmed present in the binary, incl. `mul_overflow`,
`negate_overflow`, `out_of_bounds`). Every result cross-checked against an
independent hand/numpy computation. `ulimit -s 262144`. No source or tests edited.

## Result

**No CONFIRMED memory-safety or correctness bug found.** No asan/UBSan report was
triggered by any probe, and every numeric result matched numpy exactly, including
the adversarial extremes (INT64_MIN/INT64_MAX steps, empty ranges, 0-size axes,
0-D tensors, rank-cap overflow, reversed scatter, complex tensors).

## Findings

### LOW / SUSPECTED — theoretical signed-overflow in stride/count math for astronomical steps
- **Where:** `rangeCount` (`stdlib_tensor.hpp:804-807`, the `-step` in
  `... + (-step) - 1) / (-step)`) and `indexGeometry`
  (`stdlib_tensor.hpp:830`, `g.outStride.push_back(ax.step * ss)`).
- **What:** `resolveSlice` casts the slice step straight from `asInt` into
  `std::ptrdiff_t` with only a `step == 0` guard, so a step at/near INT64_MIN
  flows in. `-step` for `step == INT64_MIN`, and `ax.step * ss` for a step on the
  order of 1e18 with `ss > 1`, are signed-overflow / negation-overflow per the C++
  standard.
- **Reproducers tried** (all returned the *correct* numpy answer, exit 0, no
  sanitizer abort):
  - `a=arange(0,10); a[::-9223372036854775808]` → `[9.0]` (numpy: `[9.]`) ✓
  - `m=arange(0,6).reshape([2,3]); m[::-4000000000000000000]` → `[[3,4,5]]` ✓
  - `a[::9223372036854775807]` → `[0.0]` ✓
- **Why it does not manifest as a bug:** a step of that magnitude forces
  `rangeCount → 1` on any real (≤ `kMaxElems`) axis, so the (possibly-overflowed)
  `outStride` value is only ever multiplied by coordinate 0 and never reaches a
  memory offset; and the count itself came out correct in every probe. I could
  **not** get UBSan to abort despite `-fno-sanitize-recover=all` and the handlers
  being linked — the specific patterns are either optimized to a non-trapping form
  at `-O1` or the negation is realized in a width that doesn't trap. **SUSPECTED,
  not CONFIRMED**: it is a real standards-level UB in the source text, but it is
  unreachable as an OOB and produced no observable wrong result or trap.
- **Note (out of scope, adjacent):** the VM lexer/arithmetic silently wraps int64
  overflow (`9223372036854775808` parses to INT64_MIN; `4000000000*4000000000`
  yields a wrapped negative) without a UBSan trap — i.e. VM integer math is done in
  a defined (wrapping) form. This is why the extreme step *values* exist at all;
  flagged only for context, it is not part of the tensor-indexing scope.

## Verified CORRECT (probed, sound)

Read/gather (`getItem` basic path), all vs. independent/numpy computation:
- `t=arange(0,24).reshape([2,3,4])`: `t[1,:,2]=[14,18,22]`; `t[:,1:3,::-1]` full
  reversed-inner block; `t[...,0]=[[0,4,8],[12,16,20]]`; `t[-1,-1,-1]=23`.
- newaxis: `m[:,None,:].shape=[2,1,3]`, `m[None].shape=[1,2,3]`,
  `t[...,None].shape=[2,3,4,1]`, `t[None,...].shape=[1,2,3,4]`,
  `t[:,None,...,None].shape=[2,1,3,4,1]`, `v[None,:,None].shape=[1,4,1]`. NewAxis
  gets outStride 0 and never perturbs the offset.
- ellipsis: middle/leading/trailing, absorbing 0 axes; `t[0,...,1]=[1,5,9]`,
  `t[0,1,...]=[4,5,6,7]`; 0-D unwrap `t[1,2,3,...]=23.0` (Float scalar),
  `t[...,1,2,3]=23.0`.
- negative step / reversal: `m[::-1]`, `m[::-1,::-1]`, `t[::-1,::-1,::-1][0,0,0]=23`,
  `t[1,...,::-1]`, `t[-1,None,::-1,-1]=[[23,19,15]]`, `t[::-1,2,None]`, `a[::-3]`,
  `a[8:0:-2]=[8,6,4,2]`. Large: `arange(1e6).reshape([1000,1000])[::-1,::-1]`
  first/last/corner elements exact, no asan report.
- empty / clamped ranges: `m[1:1]→[0,3]`, `m[:,5:2]→[2,0]`, `m[-100::-1]→[0,3]`
  (empty), `m[100:]→[0,3]`, `m[:,-100:100]` = whole. Negative-step start clamps to
  ≥ 0 whenever count > 0 (start = -1 ⇒ count 0), so no negative offset.
- 0-D tensors: `z[None].shape=[1]`, `z[None,None]=[1,1]`.
- 0-size axes: `zeros([2,0,3])` sliced/reversed/`[...,0]` all give correct empty
  shapes; integer index into a 0-length axis correctly throws "index out of range".

Write/scatter (`setItem` basic path):
- scalar broadcast `m[:,1]=99`; block `n[0:2,1:3]=[[10,11],[12,13]]`; reversed
  region `r[:,::-1]=arange` and `r[::-1]=[1,2,3,4]→[4,3,2,1]` — element order
  follows output coordinates correctly; newaxis LHS `q[:,None,:]=...`; scalar-tensor
  RHS broadcast `s[:,:]=Tensor([5.0])`; complex tensor scatter (scalar + row).
- **No partial write before an error:** shape-mismatch
  (`a[0:2,0:2]=Tensor([[..3 wide..]])`) and Complex-into-Float
  (`c[:,0]=Complex tensor`) are both detected *before* the write loop
  (`stdlib_tensor.hpp:1776`, `:1786`); target verified byte-for-byte unchanged.
- grad-tensor refusal: both basic-index and full-integer assignment on a
  `requiresgrad=True` tensor throw the intended "…desync the autograd graph…" error
  (`stdlib_tensor.hpp:1758`) before any mutation.

Parse / bounds guards (`buildIndexPlan`):
- too-many-indices (`consuming > nd`) throws; >1 ellipsis throws; `step==0` throws;
  negative int index out of range throws. `sh[axis]` access proven in-bounds:
  `consuming ≤ nd`, NewAxis doesn't advance `axis`, ellipsis fills exactly
  `nd - consuming`, and the source axis is read before increment ⇒ `axis < nd`.
- rank cap: 70 `None` keys on a 0-D tensor correctly throws
  "Tensor has too many dimensions (max 64)" on the read (output allocated via
  `checkedNumel`). On the *write* path there is no output allocation and thus no
  rank check, but the extra dims are all size-1 stride-0 (single write to offset 0),
  so it is safe — verified `v[<70×None>]=5.0` writes the single element with no
  crash. (Consistency nit, not a safety issue.)

Overflow reasoning (confirmed bounded, not merely read): every offset term
`start*ss`, `idx*ss`, and the `constOff` sum is bounded by `numel ≤ kMaxElems =
64M` for any *allocatable* tensor, so `constOff` and every produced offset stay far
below INT64_MAX and non-negative; `basicIndex`'s `t.data[(size_t)off]` therefore
never wraps to a huge index. The only unbounded quantity is a pathological step
(above), which cannot influence a real offset because it collapses the count to 1.
