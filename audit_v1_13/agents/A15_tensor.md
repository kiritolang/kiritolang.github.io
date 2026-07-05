# A15 — Tensor + N-dim Engine + Autograd Audit

**Agent:** A15
**Area:** `src/kirito/stdlib_tensor.hpp` (2487 lines), `src/kirito/tensor.hpp` (586 lines)
**Tests:** `tools/tests/unit/test_tensor.cpp`, `test_tensor_deep.cpp`, `test_multi_index.cpp`, scripts in `tools/tests/scripts`
**Method:** static reasoning, read-only. Findings below: confirmed bugs first, then coverage gaps.

---

## Surface enumerated (for reference)

Engine (`tensor.hpp`): `numel`/`checkedNumel` (caps: `kMaxElems=64Mi`, `kMaxRank=64`), `rowMajorStrides`, `broadcastShapes`, `Tensor<T>` (fill ctor is checked; **`(Shape, vector)` ctor is NOT checked** — see A15-4), `flatIndex/at`, `reshape/flatten/permute/transpose`, `mapUnary/elementwise/add/sub/mul/div/scalarOp/negate`, `matmul` (2-D + batched broadcast), `dot`, `sumAll/prodAll/minAll/maxAll/reduceAxis`, `trace/maxAbsElem/determinant/inverse` (partial-pivot; scale-relative singular tol; non-finite guarded), `unravel`, `slicePicks/sliceAxis/flip/concatenate/takeAxis0/cumulative`, `solve/outer/kron`.

Kirito layer (`stdlib_tensor.hpp`): `TensorVal` (variant Float/Complex + autograd `requiresGrad/grad/node`), grad node graph, `TensorGradFlag` (VM-scoped `_grad`), grad helpers (`sumTo/broadcastTo/transposeLast2/makeAutogradFloat`), differentiable primitives (`g_binop/g_scalar/g_neg/g_matmul/g_transpose/g_permute/g_reshape/g_flatten/g_sum/g_mean/g_math(26 ops)/g_pow/g_powT/g_where/g_clip/g_maxmin/g_sliceAxis/g_takeAxis0/g_flip/g_broadcastToShape/g_concat/g_stack/g_split/g_squeeze/g_expandDims/g_swapaxes/cumOp`), `tensordot/contract/inner/einsum`, forward-only ops (`reduceMinMax/argMinMax/stdVar/allAny/ptpT/medianT/sortT/argsortT/uniqueT/nonzeroT/searchsortedT/detT/traceT/invT/solveT/normT/outerT/kronT/crossT`), complex helpers (`realT/imagT/conjT/angleT`), `runBackward`, ctors (`Tensor/zeros/ones/full/eye/arange/linspace/*like/diag/tril/triu`), indexing (`getItem` scalar/partial/negative/boolean-mask/fancy; `setItem`; `slice` protocol axis-0). `NoGradCtx`.

## Findings

### A15-1: Stale intermediate-node gradients pollute a second `backward()` on a retained non-leaf tensor
- **severity:** medium-high
- **location:** `src/kirito/stdlib_tensor.hpp:700-745` (`accumulateGrad` / `runBackward`)
- **category:** autograd correctness (silent wrong gradient)
- **description:** `runBackward` uses each node's own `tv.grad` as the reverse-mode accumulator and NEVER clears the `.grad` of intermediate (non-leaf, `node != null`) tensors at the start of a pass. `accumulateGrad` only ADDS. `zerograd()` clears only the tensor it is called on. So if a user retains a handle to an intermediate tensor `y` (one that has a `node`) and drives `backward()` through it more than once, `y.grad` from the first pass is still present and is added to the incoming seed on the second pass, then re-propagated — doubling (or worse) the gradient that flows into the leaves. The existing tests only ever reuse **leaves** across `backward()` calls (a fresh `sum()`/`mean()` node is built each time, e.g. r7_tensor.ki:119-121, test_tensor.cpp:129), so the bug is entirely uncovered.
- **failure-scenario:**
  ```
  var x = T.Tensor([1.0, 2.0], requiresgrad = True)
  var y = x * x                 # intermediate, has a node, requiresGrad
  y.sum().backward()            # x.grad = [2,4]; y.grad set to [1,1] during traversal
  x.zerograd()                  # clears x only; y.grad stays [1,1]
  y.sum().backward()            # 2nd pass: new sum node contributes [1,1] to y ->
                                # y.grad becomes [2,2] (stale + new), y's square-backward
                                # yields x.grad = 2x*[2,2] = [4,8]  (expected [2,4])
  ```
  Also affects calling `y.backward(seed)` twice directly (an intermediate as root): the root's own `.grad` accumulates and is re-propagated, so leaf grads scale super-linearly instead of linearly.
- **proposed-test:** the snippet above, asserting `x.grad == [2.0,4.0]` after the second pass; plus a variant that calls `y.backward(T.ones(...))` twice and checks the leaf grad is exactly 2× a single pass, not 3×.
- **proposed-fix:** at the top of `runBackward`, after building `order`, reset `tv.grad` on every node in `order` that has a `node` (all non-leaves, including the root if it is one) BEFORE seeding; or accumulate into a local `unordered_map<Handle,FT>` and only ever persist `.grad` onto leaf tensors (PyTorch semantics — non-leaves don't retain grad). Leaf accumulation across calls (relied on by tests) must remain.
- **confidence:** high (mechanism verified by static trace; no test exercises the path)

### A15-2: Most differentiable ops' backward rules are NOT finite-difference gradient-checked
- **severity:** medium (coverage)
- **location:** tests — `tools/tests/scripts/r4_tensor.ki:537-599` is the only FD gradcheck harness
- **category:** coverage gap (autograd)
- **description:** The FD `fdcheck` harness numerically verifies only `exp`, `x*x`, `tanh`, `sigmoid`, `log`, and `matmul`. The remaining differentiable ops are checked only against hand-written expected constants (relu mask, clip mask, product rule, broadcast sum) or not at all. No FD check exists for: `sub`/`div`/`neg` backward; `g_pow`(scalar) and `g_powT`(tensor**tensor) backward; the math ops `log10/log2/sqrt/cbrt/square/reciprocal/abs/sign/sin/cos/tan/asin/acos/atan/sinh/cosh/asinh/acosh/atanh/softplus/erf`; `mean` backward; `cumsum` backward; `maximum/minimum` backward; `where` backward (gradient magnitudes); `clip` backward; and the structural backwards `permute/transpose/reshape/flatten/swapaxes/squeeze/expanddims/flip/broadcastto/slice/take/concatenate/stack/split`. A sign error or wrong-shape reduction in any of these would ship silently.
- **failure-scenario:** e.g. a bug in `g_binop`'s `'/'` db rule (`-g*a/b^2`) or in `sumTo`'s broadcast reduction would not be caught by any current test.
- **proposed-test:** extend `fdcheck` to cover every unary math op, the `/`/`-` binary backwards, `pow`, `maximum/minimum`, `where`, `clip`, `mean(axis)`, `cumsum`, and each structural op (reshape/permute/flip/slice/take/concat/stack/split/broadcastto) via central differences on a small tensor.
- **proposed-fix:** N/A (test coverage).
- **confidence:** high

### A15-3: Op × dtype × shape matrix has systematic Complex-branch and empty/broadcast-edge gaps
- **severity:** low-medium (coverage)
- **location:** tests broadly; branches in `stdlib_tensor.hpp`
- **category:** coverage gap
- **description:** Adversarial dtype/shape combinations that hit distinct code paths are thinly tested:
  - **Empty tensors** (a `0` in the shape → `data.empty()`): `sum/mean(axis)` over a zero-length axis (should throw "zero-size reduction"), `argmin/argmax` (guarded by `data.empty()` throw), `median/std/var` over an empty axis, `sort/argsort` of an empty axis, `nonzero`/`unique` of empty, `norm` of empty (ord 0/2/±inf), `matmul` with a 0 inner dim, `concatenate`/`stack` including an empty part. Most are correct-by-construction but untested.
  - **Complex** paths for `broadcastto` (line 995), `flip` multi-axis (986), `prod(axis)` (1780), `cumprod` (1219), `searchsorted` (throws), `nonzero` (1281), `solve` (1325), `det/trace/inv` complex, `repeat/tile` complex (1914/1924), boolean-mask indexing complex (1553). test_tensor_deep.cpp covers a subset (transpose/flatten/apply/index/reduceAxis/conjugate/astype) but not these.
  - **Broadcast-mismatch** error messages for `where`, `maximum/minimum`, `g_powT`, elementwise `%`//`//` on incompatible shapes.
  - **NaN/inf**: `sort/median/argsort/unique` NaN-last ordering (comparator is correct — valid strict-weak-order — but untested); `det/inv` non-finite rejection (guarded); reductions with inf.
- **proposed-test:** a dedicated `spec_tensor_edges.ki` enumerating empty-axis reductions (expect throws), each complex-dtype method once, broadcast-mismatch throws, and NaN ordering results.
- **proposed-fix:** N/A (coverage).
- **confidence:** high

### A15-4: Engine `Tensor(Shape, vector)` ctor and `reshape` bypass the rank cap (`kMaxRank`)
- **severity:** low (engine-only; Kirito layer is guarded)
- **location:** `src/kirito/tensor.hpp:105-107` (`(Shape,vector)` ctor) and `:131-139` (`reshape`/`flatten`)
- **category:** guard gap / stale comment
- **description:** The `(Shape, std::vector<T>)` ctor validates only `data.size() != numel(shape)` using **unchecked** `numel`, never `checkedNumel`. Thus `reshape` (which routes through this ctor) can produce a tensor with rank > `kMaxRank=64` as long as `numel` is unchanged — e.g. reshape a size-1 tensor to a shape of 100 ones. The comment at `tensor.hpp:50-53` explicitly claims reshape is covered by `checkedNumel` ("ops that build a high-rank shape from a valid one (reshape/expanddims/broadcastto) are covered too"), which is **false at the engine level**. The recursive traversals (`str`/`tolist`/indexing) that the rank cap is meant to protect could then overflow the native stack for a pure-C++/third-party engine user. Inside Kirito the risk is contained because `tns::make()` re-runs `checkSize` (→ `checkedNumel`, which enforces the rank cap) on every tensor handed to the VM, so `g_reshape` is safe. Also `numel` (`tensor.hpp:36-40`) can silently overflow `size_t` for a hand-built shape, but every Kirito path funnels through `checkedNumel` first.
- **failure-scenario:** C++ code: `tensor::reshape(Tensor<double>({1},{1.0}), Shape(100,1))` yields a rank-100 tensor with no error; a later recursive `format`/`unravel` risks stack growth.
- **proposed-test:** engine-level `CHECK_THROWS(tensor::reshape(t1, Shape(200,1)))` once the ctor is hardened.
- **proposed-fix:** route the `(Shape,vector)` ctor's size validation through `checkedNumel(shape)` (which also enforces the rank cap), and/or add an explicit rank check in `reshape`. Update the tensor.hpp:50-53 comment to match reality until then.
- **confidence:** high (code path verified)

### A15-5: `concatenate`/`split` reject negative axis while `stack`/`slice`/`take`/reductions accept it (inconsistency)
- **severity:** low
- **location:** `stdlib_tensor.hpp:2393` (`concatenate` casts axis to `size_t`), `:2407` (`split` casts to `size_t`); contrast `:2400` `stack` (keeps int64 → `g_stack` normalizes negatives), `slice`/`take`/reduction `axisOf` (all support NumPy negative axes)
- **category:** API inconsistency
- **description:** `concatenate` and `split` do `static_cast<std::size_t>(asInt("axis"))`, so a negative axis wraps to a huge value and throws "axis out of range" rather than counting from the end. Every other axis-taking op supports negative axes. `T.concatenate([a,b], -1)` fails where `T.stack([a,b], -1)` works.
- **failure-scenario:** `T.concatenate([T.Tensor([[1,2]]), T.Tensor([[3,4]])], -1)` throws instead of concatenating along the last axis.
- **proposed-test:** assert negative-axis concatenate/split match their positive equivalents.
- **proposed-fix:** normalize negative axis (add `ndim`) in the `concatenate`/`split` module fns before the `size_t` cast, mirroring `axisOf`.
- **confidence:** high

### A15-6: `norm` is always a flattened vector norm — no induced/Frobenius-vs-spectral matrix semantics
- **severity:** low (doc/semantic)
- **location:** `stdlib_tensor.hpp:1332-1345` (`normT`)
- **category:** semantic gap vs NumPy
- **description:** `normT` iterates `a.data` flat regardless of rank, so `norm(matrix, 2)` returns the Frobenius/entrywise 2-norm, not NumPy's spectral (largest-singular-value) matrix 2-norm; `norm(matrix, 1)`/`inf` likewise differ from NumPy's induced matrix norms (max column/row abs-sum). The module doc says only "norm" without qualifying. Fine as a documented vector-norm-over-flattened-data, but a user expecting `numpy.linalg.norm` matrix behavior gets different numbers with no error.
- **failure-scenario:** `T.norm(T.Tensor([[1,2],[3,4]]), 2)` returns `sqrt(30)≈5.477` (Frobenius), not the spectral norm `≈5.465`.
- **proposed-test:** document/assert the flattened semantics explicitly.
- **proposed-fix:** either document "always over flattened elements" in the stdlib reference, or implement matrix-norm cases for 2-D + `ord in {1, 2, inf}`.
- **confidence:** medium

### A15-7: `inner`/`cross`/`outer`/`dot` edge shapes: 0-D and length mismatches
- **severity:** low
- **location:** `stdlib_tensor.hpp:2456-2464` (`inner`), `:1357-1367` (`crossT`), `:1346-1351` (`outerT`), `tensor.hpp:266-272` (`dot`)
- **category:** edge-case behavior / coverage
- **description:** `inner` computes `tensordot` over `ndim-1` of each operand; for a 0-D operand `ndim()-1` underflows `size_t` → cast to `int64 = -1` → `tensordot` "axis out of range" (so it throws rather than returning the scalar product NumPy gives for `inner(scalar, x)`). `dot` (engine) requires both exactly 1-D and equal length (good, tested for equal case; the mismatched-length throw and the non-1-D throw are covered in test_tensor.cpp only for equal-length). `cross` flattens then requires exactly 3 elements (tested happy-path only; the "not 3-element" throw is untested). `outer` flattens any-rank operands (NumPy parity) — untested for >1-D inputs.
- **failure-scenario:** `T.inner(T.Tensor([1.0]).reshape([]) , ...)` — a 0-D operand — throws "axis out of range" instead of a scalar-product; not a crash, just a NumPy divergence.
- **proposed-test:** `CHECK_THROWS` for `cross` of non-3-vectors and `dot` of unequal-length; assert `outer` flattens 2-D inputs.
- **proposed-fix:** optional — special-case 0-D in `inner`; otherwise document.
- **confidence:** medium

### A15-8: Scalar-on-the-left arithmetic with a tensor is unsupported (`2 * t`, `10 / t`, `1 - t`)
- **severity:** low (documented language-wide invariant, but a real usability sharp edge for tensors)
- **location:** `src/kirito/runtime.hpp:2187-2192` (arithmetic operators deliberately do not reflect onto the RHS)
- **category:** usability / by-design limitation
- **description:** Because Kirito has no reflected (`_radd_`-style) dunder, `applyBinaryOp` never dispatches an arithmetic op to the right operand. With a plain number on the left and a `TensorVal` on the right, `Integer/Float::binary` handles the tensor RHS and throws. So `2 * t`, `1 - t`, `10 / t` all fail; users must write `t * 2`, `-t + 1`, `t.reciprocal() * 10`, etc. Every array library (NumPy/PyTorch) supports scalar-on-left; this is a frequent footgun for tensor code (e.g. `1 - sigmoid` in loss math). It is a documented invariant (`r11_docinvariants.ki`), so not a bug — but worth surfacing as a tensor-ergonomics limitation, and no tensor test asserts the throw so users get only a generic operand error.
- **failure-scenario:** `1.0 - T.Tensor([0.2, 0.8])` throws "unsupported operand"-style error instead of `[0.8, 0.2]`.
- **proposed-test:** assert the throw and document the `t`-on-left workaround in the tensor docs/recipes.
- **proposed-fix:** none required by the invariant; consider a targeted doc note in the tensor reference.
- **confidence:** high

### A15-9: Boolean-mask indexing requires an EXACT same-shape mask (no lower-rank / broadcast mask)
- **severity:** low
- **location:** `stdlib_tensor.hpp:1550-1562` (`getItem` mask branch)
- **category:** limitation / coverage
- **description:** `t[mask]` requires `mask->shape() == shape()` exactly and returns a flat 1-D selection. NumPy allows a mask over a prefix of axes (`a[rowmask]` selects rows). Reasonable to restrict, but untested for the throw and undocumented; also the mask is treated truthy via `elemAsComplex(i) != 0`, so a Float mask with a NaN element counts as selected (NaN != 0 is true) — a subtle surprise.
- **proposed-test:** assert `t[wrong_shape_mask]` throws; assert NaN-in-mask behavior is intended.
- **proposed-fix:** document; optionally reject NaN in a mask.
- **confidence:** medium

### A15-10: `setItem` (in-place `t[i]=v`) does not invalidate/participate in the autograd graph
- **severity:** low
- **location:** `stdlib_tensor.hpp:1603-1617` (`setItem`)
- **category:** autograd correctness (edge)
- **description:** Element assignment mutates `store` in place but leaves `requiresGrad`/`node`/`grad` untouched. Mutating a computed (non-leaf) tensor in place makes its stored value inconsistent with its recorded `node` (the graph still describes the pre-mutation value); a subsequent `backward()` through that node uses the op's captured operand copies, so gradients ignore the mutation. This is by-design (docs: arithmetic is pure, the only in-place op is element assignment, and gradient steps rebind rather than mutate), but there is no guard or warning when a user mutates a `requiresGrad`/node-bearing tensor, so a silent graph/value mismatch is possible.
- **failure-scenario:** `var y = x*x; y[0] = 99.0; y.sum().backward()` — `x.grad` reflects the un-mutated `y`, which may surprise.
- **proposed-test:** document the intended behavior; optionally `warnDetach`-style warn on in-place assignment to a node-bearing tensor.
- **proposed-fix:** optional warn-once when `setItem` targets a tensor with a `node`.
- **confidence:** medium

### A15-11: DRY — Complex arithmetic dispatch duplicated between `TensorVal::binary` and the Float `g_binop`
- **severity:** low (informational)
- **location:** `stdlib_tensor.hpp:1505-1521` (`binary`) vs `:317-350` (`g_binop`) and `tensor.hpp:203-222`
- **category:** DRY / maintainability
- **description:** The `+ - * /` selection is spelled three times: the `char c = ...` + `switch(c)` in `binary` for the complex/scalar-complex paths, `g_binop`'s `op == '+' ? ... : ...` chain for the Float grad path, and the engine's `add/sub/mul/div`. The complex branch necessarily bypasses autograd, so some split is justified, but the operator-char plumbing (`'+','-','*','/'`) and the tensor-vs-scalar fan-out are near-identical across paths and easy to drift. The `matrix`/`complex` matrix types correctly reuse the engine (`determinant/inverse/trace/matmul` in `tensor.hpp`), so that layering is clean; the duplication is only in the Kirito dispatch glue.
- **proposed-fix:** factor a single `applyTensorArith(vm, char op, ...)` that branches Float(grad)/Complex once; low priority.
- **confidence:** medium

## Summary of coverage strengths (to avoid over-flagging)
- Engine core (shape/stride/broadcast/matmul/reduce/linalg/slice/concat/take/cumulative/solve/outer/kron) is directly unit-tested (test_tensor.cpp) including complex-dtype determinant and unordered-min throw.
- Resource caps are consistently applied at allocation via `checkedNumel`/`tns::checkSize`, and the size-overflow multiply guards (`eye`, `diag`, `repeat`, `tile`, `einsum` `total`, `arange`) are individually present and commented — these are genuinely well-hardened (verified no `size_t` product wraps to a small allocation with an OOB write; `diag(INT64_MIN)` and huge `repeat/tile` throw "Tensor too large" cleanly).
- Domain guards (`log/sqrt/asin/acos/acosh/atanh/reciprocal/pow`) throw instead of emitting NaN/inf, with parity to the scalar `math` module (spec_domain_guards.ki, test_tensor_deep.cpp).
- `einsum` rejects repeated/undefined output labels and overflowing contractions; NaN sort ordering uses a valid strict-weak-order comparator.
- The iterative (non-recursive) `runBackward` DFS correctly avoids native-stack overflow on deep graphs, and `str`/`tolist` recursion is rank-capped (within the Kirito layer).
