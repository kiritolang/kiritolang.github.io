# A4 — NUMERICS / TENSOR — deep audit (scan3, v1.17.1)

Scope: BigInt (`stdlib_int.hpp`), Integer/Float arithmetic corners, complex (`stdlib_complex.hpp`),
matrix (`stdlib_matrix.hpp`), tensor engine (`tensor.hpp`), the Kirito tensor wrapper + slicing +
autograd (`stdlib_tensor.hpp`). All findings reproduced on `build-bin/ki-asan`. Every numeric answer
cross-checked by an independent hand computation and, where possible, against CPython
(`math`/`pow`/`gcd`/`isqrt`).

Hard rule applied: only user-code-provable defects are counted; each below has a `.ki` reproducer.

## Summary
- CONFIRMED: 1 (0 HIGH, 1 MED, 0 LOW)
- Everything else probed is CORRECT. Autograd (forward + reverse) matches finite-difference and analytic
  derivatives across the full op set; BigInt matches CPython exactly at large magnitude; complex, matmul,
  slicing (incl. INT64 steps), reductions, det/inverse all verified.

---

## FINDING A4-1 · MED · CONFIRMED
`stdlib_tensor.hpp:2447-2481` (the `arange` module fn, specifically the accumulation loops at
2477-2478).

`arange(start, stop, step)` with a non-integer `step` produces **one spurious extra element** and
diverges from the NumPy model the module advertises. The half-open contract ("stop (exclusive)",
docs `30-bonus-05-tensors.md:55` and the code's own comment) is visibly violated: the final element is
≈ `stop`.

Reproducer:
```
var t = import("tensor")
var a = t.arange(0.0, 1.0, 0.1)
# a.size()            -> 11        (NumPy: np.arange(0,1,0.1) has length 10)
# a[a.size()-1]       -> 0.9999999999999999  (prints "1.0"; < stop, so it slips into [start,stop))
```
Actual: 11 elements `[0.0, 0.1, ..., 0.8999999999999999, 0.9999999999999999]`.
Independently-computed expected (NumPy semantics): 10 elements, count = `ceil((stop-start)/step)` =
`ceil(1.0/0.1 == 9.999999999999998)` = 10, values `start + i*step` for `i in [0,10)`, last = `0.9`.

Root cause: the element count and the element values are produced two different ways. The size cap uses
the correct count `countf = ceil(span/step)` (line 2472, evaluates to 10 here) — but the actual fill
loops `for (double x = start; x < stop; x += step)` (2477-2478), i.e. Kahan-free running accumulation.
After ten additions the accumulator is `0.9999999999999999 < 1.0`, so an 11th element is appended; the
`reserve(countf)` of 10 is silently outgrown. The result length is thus non-deterministic w.r.t. float
error and inconsistent with the count the same function already computed. NumPy deliberately derives the
length from the `ceil` count and emits `start + i*step` precisely to avoid this endpoint leak.

Suggested fix (behavior change — flag for maintainer): drive the loop by the already-computed integer
count and emit `start + i*step` (matches NumPy, removes the spurious endpoint, makes length
deterministic). Add a `.ki` regression named e.g. `arange_fractional_step_no_endpoint_leak`.

Severity rationale: a wrong array length / off-by-one from a discretization grid is a real wrong result
that propagates downstream; but it only bites non-integer steps and stays memory-safe, so MED not HIGH.

---

## Verified CORRECT (with the independent cross-checks run)

### Autograd — reverse-mode vs finite-difference (eps=1e-6, central) and vs analytic
All matched to ~1e-9. Ops covered: `exp, log, log2, log10, sqrt, tanh, sin*cos, sigmoid, softplus, erf,
pow(p), x**y (both operands), div, clip, maximum, relu/abs (via analytic), square`.
- `exp` grad = exp(x); `pow3` grad = 3x² → [0.75,4.32,12,0.03] ✓; `log(x²+1)` grad = 2x/(x²+1) ✓;
  `sqrt` grad = 0.5/√x ✓; `2**b` grad = 2^b·ln2 ✓; `clip` grad = 1 inside bounds else 0 ✓.
- Structural / linear-algebra gradients hand-verified:
  - matmul: dA = 𝟙@Bᵀ (row-sums of B) = [[3,7,11],[3,7,11]]; dB = Aᵀ@𝟙 (col-sums of A) ✓.
  - broadcasting (2,1)·(1,3): dx = Σⱼyⱼ = 111; dy = Σᵢxᵢ = 5 ✓ (sum_to correct).
  - axis-sum / mean gradients (scaled) ✓; slice / strided-slice / take(with repeats) scatter-add ✓;
    concatenate / stack split-back ✓; tensordot (=matmul) ✓.
- Semantics: shared-subgraph accumulation (d(x²+x)=2x+1) ✓; two-branch reuse ✓; leaf grad accumulates
  across `backward()` calls, `zerograd()` resets, non-leaf grad is cleared per PyTorch ✓.
- `setItem` on a grad-tracking tensor hard-errors (stale-cache guard) ✓.

### Tensor `%` and `//` (floor semantics, sign of divisor)
`[7,-7,7,-7] %/// [3,3,-3,-3]` → mod `[1,2,-2,-1]`, floordiv `[2,-3,-3,2]`, and
`(a//b)*b + a%b == a` ✓ — matches Python floor-mod hand computation; scalar forms agree.

### BigInt — exact vs CPython
`factorial(50)`, `isqrt(10^30)`, `gcd`, `modpow(...,1e15+37)`, `modinv(...,1e12+39)`, `7^100`,
`comb(200,100)`, `(10^20-1)²`, `isqrt(152415787532388367504953515625)` — **all bit-identical to Python**.
Floor div/mod signs match Python for all four sign combos; identity holds. `isqrt` exact at
square−1/square/square+1 boundaries. `0**0=1`. Error paths clean+diagnostic: div-by-zero, non-coprime
modinv, factorial(neg), isqrt(neg), `2**-3 → 0.125` (Float). Size caps throw catchably.

### Complex + ComplexMatrix
`|3+4i|=5`, `arg=0.9273`, `sqrt(-1)=i`, `exp(iπ)=-1`, `log(-1)=iπ` ✓. ComplexMatrix det
`(1+1i)·1−2·(0+1i)=1−1i` ✓; `M·M⁻¹ = I` ✓; Hermitian dot `conj(u)·v = i` ✓.

### Slicing / multi-axis basic indexing
`m[:,2:4]`, `m[:,::-1]`, `m[...,0]`, `m[:,None]` (shape [3,1,4]), `m[::-1,:]`, empty `m[2:2,:]` /
`m[:,3:1]` ✓. INT64_MAX step → single element; INT64_MIN step (`-9223372036854775808`) → correct last
element (no negation UB). Slice/scalar/tensor assignment into strided regions ✓. asan clean throughout.

### Reductions / stats / structural
Empty-axis sum→0, prod→1, all→1 (vacuous), any→0; empty-axis min/max and no-identity reduction throw.
`einsum` (matmul/trace/transpose/diagonal/dot) ✓; `median` odd/even ✓; `var` pop=1.25 / sample(ddof=1)=
5/3 ✓; NaN propagation in max/min and first-NaN in argmax/argmin ✓; `sort`/`argsort`/`unique`/`median`
NaN-sorts-last comparator is a valid strict-weak-ordering (verified no asan/UBSan hit); `norm` ord
2/1/0/±inf ✓; `round` = round-half-to-even ([0,2,2,4,-0,-2]) ✓; floor/ceil/trunc/sign ✓;
cumsum/cumprod ✓; batched matmul (2,2,3)@(3,2) ✓; matmul with k=0 inner dim → zeros; repeat/tile ✓;
`broadcastto` shrink errors; boolean-mask and fancy indexing ✓; searchsorted ✓; where ✓; astype ✓;
Tensor serialize round-trip (`serialize.dumps/loads`) ✓.

### Notes (informational, not counted)
- `median`/`sort` place NaN last rather than returning NaN (NumPy returns NaN). This is a **documented,
  deliberate** prior decision (pinned in `r7_regressions`; comment at `stdlib_tensor.hpp:1349`), applied
  consistently across sort/argsort/unique/median — not a defect.
- BigInt follows the reflected-operator rule (`3 + BigInt` throws); by design, not a bug.
