# A13 — tensor + matrix + complex Audit (v1.14)

**Agent:** A13
**Area:** src/kirito/tensor.hpp, stdlib_tensor.hpp, stdlib_matrix.hpp, stdlib_complex.hpp
**Method:** static read + `.ki` probes CONFIRMED against `build-debug/ki`. READ-ONLY on src/.
**Status:** COMPLETE

Prior audits merged: v1.13 A15 (tensor) + A16 (matrix/complex), v1.12. Several v1.13 findings were
FIXED (verified below); the residue + new angles are recorded here.

## Verified-fixed since v1.13 (not re-flagged)
- **A15-1 (stale intermediate grad on 2nd backward): FIXED.** `runBackward` now resets `.grad` on
  every node that has a `.node` before seeding (stdlib_tensor.hpp:742), PyTorch non-leaf semantics.
  Probe: `y=x*x; y.sum().backward(); x.zerograd(); y.sum().backward()` → `x.grad==[2,4]` (correct).
- **A16-1 (real-matrix `apply` un-rooted arg / GC bug): FIXED.** stdlib_matrix.hpp:243-244 now wraps
  the callback arg in `RootScope rs(vm); rs.add(vm.makeFloat(...))`, matching the complex sibling.
- Tensor scalar AND element-wise `//`/`%` by zero throw cleanly ("tensor floor-division/modulo by
  zero"); div by zero throws; clip with lo>hi throws; empty-axis reduction throws; whole-tensor `==`
  is single Bool with NaN-never-equal (Float and Complex); `item()` on non-scalar throws; mask/fancy
  out-of-range throw; partial index returns a COPY (pure model). All confirmed good.

---

## Findings

### A13-1: `min`/`max`/`argmin`/`argmax` give ORDER-DEPENDENT results when a NaN is present (NEW)
- **severity:** medium
- **location:** `tensor.hpp:279-298` (`minAll`/`maxAll`), `tensor.hpp:302-328` (`reduceAxis` seeded
  with first element), `stdlib_tensor.hpp:1113-1154` (`reduceMinMax`/`argMinMax`, the `v > bv`/`v < bv`
  comparisons and `std::max`/`std::min`).
- **category:** correctness / silent wrong result (order-sensitivity)
- **description:** These reductions seed the accumulator with the FIRST element and update via
  `v > bv` / `v < bv` (or `std::max`/`std::min`). Because every comparison against NaN is false, a NaN
  is *silently skipped* — UNLESS it is the seed (first element), in which case nothing ever beats it and
  it is returned. So the result depends on the POSITION of the NaN, not just its presence. A `max` should
  be order-invariant; here it is not. This also makes the library **internally inconsistent** (`sum`/
  `mean`/`std`/`var`/`prod` propagate NaN → `nan`; `min`/`max`/`argmin`/`argmax`/`median`/`sort` silently
  drop it) and **divergent from NumPy** (which returns `nan` / the NaN index for all of these).
- **failure-scenario (confirmed):**
  ```
  T.Tensor([nan,1,3]).max() -> nan      T.Tensor([1,nan,3]).max() -> 3.0    T.Tensor([1,3,nan]).max() -> 3.0
  T.Tensor([nan,1,3]).argmax() -> 0     T.Tensor([1,3,nan]).argmax() -> 1
  T.Tensor([[nan,1],[1,nan]]).max(0) -> [nan, 1.0]   # per-axis: same multiset, different answer per column
  ```
  A user validating data with `if isnan(t.mean())` is fooled because `t.max()` of the same data hides
  the NaN behind a plausible finite value; and the answer flips if the data is merely reordered.
- **proposed-test:** assert `max`/`min`/`argmax`/`argmin` on `[nan,1,3]`, `[1,nan,3]`, `[1,3,nan]` are
  identical (all `nan` / the NaN index) — pins whichever policy is chosen. None exists today.
- **proposed-fix:** pick ONE policy and make it order-invariant: either propagate NaN (NumPy: check for
  NaN and short-circuit to `nan` / NaN index — matches `sum`/`mean`) or nan-skip everywhere (nanmax-style,
  matching `median`/`sort`). Whichever, the reduction must not depend on element order.
- **confidence:** high (mechanism read in source + all cases reproduced).

### A13-2: `complex.atan` (and `atanh`'s mirror) not guarded at the ±i branch-point poles (carry A16-2, STILL UNFIXED)
- **severity:** low-medium
- **location:** `stdlib_complex.hpp:591` — `unary("atan", std::atan)` registered with NO `bad`
  domain predicate; contrast `atanh` at 597-601 which guards ±1.
- **category:** domain-guard gap (silent inf/nan vs the documented "throw at genuine singularities")
- **description:** `atan(z)` has genuine poles at `z = ±i` (`atan = (1/2i)·log((1+iz)/(1-iz))`). The
  documented policy (header comment 565-571 + CLAUDE.md) is to throw a `math domain error` at genuine
  singularities (log/log10 of 0, atanh of ±1) rather than emit silent inf/nan. `atanh(±1)` is guarded;
  its analytic mirror `atan(±i)` is not.
- **failure-scenario (confirmed):** `complex.atan(complex.i)` → `0.0+infi`;
  `complex.atan(complex.of(0,-1))` → `0.0-infi`. No error.
- **proposed-test:** `throws(Function(): return C.atan(C.i))` and `...C.atan(C.of(0,-1))`, mirroring
  the existing `atanh(±1) throws` tests (r4_linalg.ki:293-294). Only `atan(0)=0` is tested today.
- **proposed-fix:** add a `bad` predicate to the `atan` registration returning
  `"math domain error (atan of ±i)"` when `z == cdouble(0,1) || z == cdouble(0,-1)`, symmetric with atanh.
- **confidence:** high.

### A13-3: `MatrixVal::_setstate_` lacks the negative-dimension guard that `ComplexMatrixVal` has (carry A16-3, STILL UNFIXED)
- **severity:** low
- **location:** `stdlib_matrix.hpp:306-307` casts `items[0]/items[1].asInt(...)` straight to `size_t`
  with no sign check; the complex sibling `stdlib_complex.hpp:494-495` rejects `r64<0||c64<0`.
- **category:** robustness / deserialization hardening (divergence from the complex path)
- **description:** A corrupt/hand-crafted `dump`/`serialize` blob with a negative `cols` becomes a huge
  `size_t`. Most values are caught downstream (the `r > kMaxMatrixElems/c` cap when `c!=0`, or the
  `data.size()!=r*c` check), but the corner `cols==0, data=[]` slips through: `c==0` skips the cap and
  `r*c==0` matches empty data, so a Matrix with a nonsense row count is built.
- **failure-scenario (confirmed):** `M.zeros(1,1)._setstate_([-1, 0, []])` → a Matrix whose `rows()`
  reports `-1` (a 2^63-scale garbage count) instead of a clean "malformed state" throw.
- **proposed-test:** feed `_setstate_` a `[-1, 0, []]` state and assert it throws (mirror of the complex
  path, which already rejects it).
- **proposed-fix:** mirror stdlib_complex.hpp:494-495 — read into signed locals and throw
  `"Matrix _setstate_: dimensions must be non-negative"` before the `size_t` cast.
- **confidence:** high.

### A13-4: `Complex` has value equality but no `hash` → unhashable, can't be a Set/Dict key (carry A16-4, STILL PRESENT)
- **severity:** low
- **location:** `stdlib_complex.hpp:65-76` (`ComplexVal::equals`, incl. cross-type `Complex(2,0)==2`);
  no `hash()` override, so `Object::hash()` throws "unhashable type 'Complex'".
- **category:** consistency gap vs the value model ("values are hashable where it makes sense")
- **description:** `Complex` is a pure immutable value with full `==` (including `Complex(2,0)==2`) yet
  cannot key a Set/Dict. Python's `complex` is hashable and hash-agrees with the equal real. `DateTime`
  (stdlib_time.hpp) is the intended precedent (override both `equals` and `hash`). Not a correctness bug
  (it's unhashable, not identity-hashed, so no eq/hash contract violation) — a missing capability.
- **failure-scenario (confirmed):** `var s = {complex.i}` → "unhashable type 'Complex'".
- **proposed-fix:** add `hash()` combining re/im, hashing `Complex(x,0)` identically to the real `x` so
  the cross-type `equals` stays hash-consistent. Or document the intentional non-hashability + pin it
  with a test (currently the behavior is unpinned — neither hashability nor the throw is asserted).
- **confidence:** medium (design call; observation solid).

### A13-5: `reshape` does not support NumPy's `-1` dimension inference (NEW, minor)
- **severity:** low (usability / NumPy divergence)
- **location:** `stdlib_tensor.hpp` reshape wrapper → rejects negative dims ("shape dimensions must be
  non-negative") before reaching `tensor::reshape`.
- **category:** usability gap
- **description:** `t.reshape([-1, 2])` — the ubiquitous NumPy "infer this axis" idiom — throws rather
  than inferring the axis from the total element count. Users must compute the dimension by hand.
- **failure-scenario (confirmed):** `T.arange(6.0).reshape([-1,2])` → "Tensor shape dimensions must be
  non-negative"; likewise `[3,-1]`.
- **proposed-fix:** support a single `-1` axis in the reshape wrapper (infer it as `numel/product(rest)`,
  error if not divisible or if more than one `-1`), OR document explicitly that `-1` inference is
  unsupported. Currently neither the support nor the limitation is documented/tested.
- **confidence:** high (reproduced); low that a change is required vs a doc note.

---

## Coverage gaps (C++ `tools/tests/unit/*.cpp` + `.ki` scripts)
- **NaN reduction semantics entirely untested** (A13-1): no `.ki` or C++ test exercises `min`/`max`/
  `argmin`/`argmax`/`median`/`sort` with a NaN element, so the order-dependence and the
  propagate-vs-drop inconsistency ship uncaught. (`r4_tensor.ki` tests min/max/argmax only on clean
  data.)
- **`complex.atan(±i)` untested** (A13-2): only `atan(0)=0` (r4_linalg.ki:276). The pole is uncovered.
- **`Matrix._setstate_` negative/adversarial dimension untested** (A13-3): serialization round-trip is
  tested only for well-formed matrices (audit_matrix.ki).
- **`Complex` as a Dict/Set key untested** (A13-4): neither hashability nor the "unhashable" error is
  asserted, so the behavior is unpinned.
- **`reshape` with `-1` untested/undocumented** (A13-5).
- test_matrix.cpp (69 lines) / test_complex.cpp (79 lines) are thin; the heavy coverage lives in the
  `.ki` scripts (r4_linalg is genuinely exhaustive for the clean-data + singular/non-square/domain
  cases). The gaps above are all NaN / adversarial-serialization / hashability edges the scripts skip.

## Non-findings verified present (defenses confirmed — avoid re-flagging)
- Element-count cap (`checkedNumel`, `kMaxElems=64Mi`, `kMaxRank=64`) single-sourced at the fill ctor;
  matrix/complex `kMaxMatrixElems=16Mi`. Singular real+complex inverse throws; non-square det/inv/trace
  throw; matmul/add/sub dimension mismatch throw; cross of non-3-vector throws; complex `<`/`>` throw
  (unordered); complex div-by-zero + `0**neg`/`0**complex` throw; det/inv reject non-finite input;
  broadcasting incompatible-shape throws; mask-shape/fancy-OOR/assign-shape-mismatch throw; slice
  step==0 throws; backward on non-scalar without seed throws, backward wrong-dtype (complex) throws,
  in-place `setItem` on a requiresgrad leaf is allowed (documented pure/rebind model). `clip(lo>hi)` throws.
- ComplexMatrix `_setstate_` DOES guard negative dims + short element pairs (the reference A13-3 should copy).

## Summary
Total: **5 findings** — 1 medium (A13-1 NaN order-dependence, NEW), 1 low-medium (A13-2 atan pole,
carry-unfixed), 3 low (A13-3 matrix setstate carry-unfixed, A13-4 Complex unhashable carry, A13-5
reshape -1 NEW). Plus 5 coverage gaps. 2 v1.13 findings verified FIXED (A15-1, A16-1).
Strongest NEW: **A13-1** — order-dependent `max`/`argmax`/`min`/`argmin` under NaN.
