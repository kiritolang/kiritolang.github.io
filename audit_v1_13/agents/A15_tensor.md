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

