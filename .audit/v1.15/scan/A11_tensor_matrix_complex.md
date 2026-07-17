# A11 Tensor/Matrix/Complex

Status: DONE

Scanner A11, v1.15 audit round. Subsystem: tensor.hpp, stdlib_tensor.hpp, stdlib_matrix.hpp, stdlib_complex.hpp.

Read .audit/README.md false-positive table first (left-scalar tensor throws, complex.polar negative rho,
Complex unhashable, exact ==, etc.) — will not re-flag those.

Initial observation: tensor.hpp itself carries inline comments referencing prior-round fixes (A11-1,
A13-1 already applied — checkedNumel/kMaxRank/kMaxElems guards, NaN-propagating min/max, broadcast-with-
zero-axis fix, non-finite-scale rejection in det/inverse). This file looks heavily hardened already from
earlier rounds. Continuing into stdlib_tensor.hpp (autograd), stdlib_matrix.hpp, stdlib_complex.hpp.

Read stdlib_tensor.hpp (full, 2519 lines), stdlib_matrix.hpp, stdlib_complex.hpp in full. Also ran a
live build (`build-debug/ki`, already configured) against ~20 adversarial one-liner probes covering:
empty-tensor sum, matmul shape mismatch, singular inverse (method-vs-module-fn), OOB index, tensor
division by zero, complex log(0), reshape-size mismatch, einsum bad/mismatched subscripts, backward on
a detached node, item() on non-scalar, ragged nested-list ctor, huge-shape guard, broadcast mismatch,
NaN propagation into determinant, 0-D tensor indexing/item, whole-tensor `==` with NaN, Complex tolist
round-trip, argmax of an empty tensor, zero-length-axis min/sum/mean, batched-matmul non-broadcastable
batch dims, non-contiguous (permuted) tensor arithmetic, setitem with a partial index, negative-axis
tensordot, out-of-range sort axis, std/var with ddof >= n, Complex-dtype requiresgrad rejection. Every
one of these produced a correct, clean, catchable `KiritoError` (or a correct numeric result) — no
crashes, no wrong answers, no silent corruption found in this pass.

This subsystem carries visible scar tissue from *five* prior audit rounds (comments cite v1.12 A13-1,
1.12.1 A11-1, v1.13 docinvariants, etc.) and reads as one of the most thoroughly hardened areas of the
codebase. The one substantive finding from this pass is A11-1 below (an inconsistency, not a crash).


## Findings

### A11-1: `t.mean(axis=k)` throws on a zero-length axis while `t.sum(axis=k)` gracefully returns 0  [LOW] [high]
- **Location**: src/kirito/stdlib_tensor.hpp:454 (`g_sum`, passes `identity = 0.0` to `reduceAxis`) vs.
  stdlib_tensor.hpp:493 (`g_mean`, calls `tensor::reduceAxis(a, ax, plus)` with **no** identity argument)
- **What**: `tensor::reduceAxis` (tensor.hpp:319-352) throws `"zero-size reduction: cannot reduce over
  an empty axis"` whenever the reduced axis has length 0 and no `identity` was supplied; it returns the
  identity value (and does not throw) when one is. `g_sum`'s per-axis path explicitly passes
  `identity = 0.0`, matching NumPy (`np.zeros((3,0)).sum(axis=1) == [0,0,0]`). `g_mean`'s per-axis path
  (stdlib_tensor.hpp:493, and its Complex sibling one line above at 474) passes no identity, so the same
  shape throws instead of returning a graceful zero/NaN. The **whole-tensor** `mean()` (axis<0 path,
  line 469/482) already has an explicit, deliberately-tested `throw` guard for n==0 — so a *consistent*
  "mean of nothing is a domain error, always throw" policy is clearly the intended design; the
  axis-reduction path simply falls into the same throw by omission. This means `sum` and `mean`
  diverge in observable behavior for the exact same input shape (`[3,0]`, `axis=1`): `sum` ->
  `[0.0, 0.0, 0.0]`, `mean` -> hard throw. That divergence is untested (grep of
  `tools/tests/scripts/*.ki` finds only whole-tensor `mean()`-of-empty tests, never
  `mean(axis=...)` over a zero-length axis specifically), so it may be an accidental gap rather than a
  reviewed decision.
- **Repro**:
  ```kirito
  var T = import("tensor")
  io.print(T.zeros([3, 0]).sum(axis=1).tolist())   # [0.0, 0.0, 0.0]
  io.print(T.zeros([3, 0]).mean(axis=1).tolist())  # throws: "cannot reduce over an empty axis"
  ```
- **Proposed fix**: N/A as a "fix" per se — throwing is arguably the *more correct* behavior (mean of
  an empty set is mathematically undefined, and the codebase's stated policy is "domain errors throw
  rather than emit silent NaN/inf"; `sum`'s identity=0 for an empty axis is arguably the odd one out).
  If the team wants `sum` and `mean` to agree in behavior, either (a) make `g_mean`'s per-axis path
  explicitly throw with the same wording as the whole-tensor path (so the throw is intentional rather
  than an emergent side-effect of omitting `identity`), or (b) leave as-is and document the asymmetry.
  Not a memory-safety or silent-corruption bug — purely a documentation/consistency question.
- **Proposed test**: pin both branches of the divergence:
  `check(T.zeros([3,0]).sum(axis=1).tolist() == [0.0,0.0,0.0], "sum empty axis -> 0")` and
  `check(throws(Function(): return T.zeros([3,0]).mean(axis=1)), "mean empty axis -> throws (unlike sum)")`.

## Coverage gaps (untested combinations found while reading; not necessarily bugs)

- `t.mean(axis=k)` / `t.std(axis=k)` / `t.var(axis=k)` over a **zero-length axis** specifically (only
  whole-tensor empty-mean is tested; per-axis empty-axis is untested for mean/std/var, though it is
  tested for sum/min/max — see A11-1).
- Complex-dtype **boolean-mask** `t[mask]` and **fancy-index** `t[[i,j]]` (`getItem`, stdlib_tensor.hpp
  ~1567-1591) — the golden scripts almost exclusively exercise boolean/fancy indexing on Float tensors;
  a dedicated Complex-dtype indexing test appears to be missing.
- `einsum` with a **repeated input label within one operand's own subscript** (a same-operand diagonal,
  e.g. `"ii->i"`) was not adversarially probed here.
- `tensor.diag(t, k)` with `k` large enough that `n = t.size() + koff` sits right at the `kMaxElems`
  boundary (the guard exists via `tns::checkSize`, but no test exercises the exact boundary).
- `ComplexMatrix` singular-inverse / non-finite-element paths (the Float `Matrix`/`Tensor` siblings were
  adversarially probed live and are correct; `ComplexMatrixVal` shares the same `tensor::inverse`/
  `tensor::determinant` templates so is very likely equally correct, but wasn't independently re-probed).
- `Matrix.apply(fn)` / `ComplexMatrix.apply(fn)` where `fn`'s return value only coerces to a number via
  a user class's dunder rather than being an actual Integer/Float/Complex — untested edge of
  `mat::numOf`/`cpx::asComplex`'s strictness.

## DRY / architecture note

Confirmed as-designed: `matrix` (`MatrixVal`) and the complex `Matrix` (`ComplexMatrixVal`) are both
thin Kirito-facing wrappers around `tensor::Tensor<double>` / `tensor::Tensor<cdouble>` respectively,
and both route `+`/`-`/`*`/`transpose`/`determinant`/`inverse`/`trace` straight through the shared
`tensor.hpp` engine rather than reimplementing linear algebra per type — exactly the single-engine
design CLAUDE.md describes. No DRY violation found; the singularity/non-finite-value guards, the
scale-relative tolerance, and the Gauss-Jordan inversion are written once in tensor.hpp and inherited by
`matrix`, `complex`'s `ComplexMatrix`, and the `tensor` module's own `.inv()`/`.det()`/`.solve()`
(via the `la1`/`la2` free-function wrappers in stdlib_tensor.hpp).

## Summary

Adversarially probed ~25 edge cases live against the built `build-debug/ki` (empty tensors, shape
mismatches, singular/non-finite matrices, OOB/negative/fancy/boolean indexing, div-by-zero, complex
domain errors, autograd detach/accumulate/seed-mismatch/nograd, 0-D tensors, NaN in `==` and reductions,
serialization round-trip) and read all four subsystem files in full. Found **zero** crashes, memory-
safety issues, or silently-wrong numeric results — this subsystem has been re-hardened across five prior
audit rounds and it shows. One real (but low-severity, arguably-by-design) inconsistency: `sum(axis=k)`
gracefully returns 0 over a zero-length axis while `mean(axis=k)` throws over the same axis (A11-1) —
flagged for a documentation/test decision rather than a code fix. Several coverage gaps noted (Complex-
dtype indexing, per-axis empty mean/std/var, einsum same-operand diagonal) for a future round's test
additions. DRY confirmed: `matrix`/`complex.Matrix`/`tensor` all share one `tensor.hpp` linear-algebra
engine.
