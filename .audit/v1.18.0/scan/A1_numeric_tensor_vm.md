# A1 — Numeric / Tensor / VM-core (dedicated from-scratch correctness hunt)

Reproduced on build-debug/ki + build-asan/ki; independent verification via python3 (statistics,
fractions) and hand computation. No HIGH. One MED.

## Finding (MED, CONFIRMED) — Tensor.round() vs scalar round() tie-break mismatch
`stdlib_tensor.hpp:539` used `std::nearbyint` (half-to-even) while `runtime.hpp:3847` scalar `round`
uses `std::llround` (half-away-from-zero). `Tensor([0.5,1.5,2.5]).round()` → `[0,2,2]` vs scalar
`[1,2,3]`. SSOT violation, tensor convention undocumented. → FIXED (unify to `std::round`, see #9).

## VERIFIED CORRECT (probed adversarially)
- Int64 two's-complement wrap (`maxint+1`, `2**63`); math overflow guards (factorial/comb/perm/prod/
  lcm/gcd) throw consistently. Floor-div/mod signs for Int+Float incl. negatives match Python.
- `Integer(float)` boundary (`9.2e18` ok, `1e19` throws); round→Integer rejects NaN/inf/out-of-range.
- statistics mean/median/variance/pvariance/mode/multimode/quantiles — byte-for-byte vs CPython incl.
  quantiles exclusive extrapolation `quantiles([1,2],4)==[0.75,1.5,2.25]`.
- Complex: sqrt(-1)=i, log(-1)=iπ, exp(iπ)=-1; branch cuts; pole guards (log0, atanh±1, atan±i,
  0**-1) throw; 0**0=1; complex-matrix inverse verified M·M⁻¹=I.
- Tensor: contiguous fast path == broadcasting path; the TWO independent slice-count impls
  (slicePicks vs rangeCount) agree with each other and Python across 9 adversarial bound cases;
  multi-axis `t[:,2:4]`, newaxis, ellipsis, neg index; 3-D reductions per axis hand-verified; empty
  sum→0/prod→1, max([]) throws; NaN sort-last / propagate consistent; linalg det/inv/solve/trace,
  Hilbert-5 solve err ~1e-11, singular→throw, non-square→throw; einsum/tensordot/diag/tril/triu/
  repeat/tile/take/searchsorted/linspace/arange; size caps + reshape-overflow clean under ASan/UBSan.
- Autograd: sum(x²)′=2x, matmul/broadcast/where/tensordot grads, leaf accumulation across two
  backward() calls; deep 50-op chain clean under KIRITO_GC_THRESHOLD=1 + ASan with fresh values.
- VM: closures capture the variable (Python semantics); deep recursion → catchable, no segfault.

## Informational (no action)
`Tensor.angle()` of NaN→0.0 / of a negative Float→π (NumPy gives nan); `argsort` of a 0-D tensor →
shape {1}. Edge divergences, not wrong numeric results in normal use.
