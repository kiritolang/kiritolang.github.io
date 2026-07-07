# v1.12.1 (audit loop) — matrix & complex subsystem

Source: `src/kirito/stdlib_matrix.hpp`, `src/kirito/stdlib_complex.hpp` (both on 2-D `tensor::Tensor`).
Determinant (Gaussian elim) / inverse (Gauss-Jordan) live in `src/kirito/tensor.hpp:369-435`.
Probe binary: `./build-debug/ki`. Verdict: **subsystem is solid**; one LOW finding (polar UB),
everything else probed is correct or documented by-design.

## FINDINGS

### F1 [LOW] `complex.polar` passes non-finite / negative args straight to `std::polar` (UB, silent NaN)
- where: src/kirito/stdlib_complex.hpp:543-546 (`m.fn("polar", ...)` -> `std::polar(r, theta)`)
- repro:
  ```
  var cx = import("complex")
  cx.polar(1, 1e308*10)   # => -nan+-nani   (theta = +inf)
  cx.polar(nan, 0)        # => -nan+-nani
  cx.polar(-1, 0)         # => -1.0+0.0i    (negative rho)
  ```
- actual: silently returns NaN/garbage (or a value from UB territory).
- expected: per `std::polar`, "if rho is negative or NaN, or theta is infinite/NaN, the behavior is
  UNDEFINED." The module's own stated philosophy (analytic set + `math` module) is to THROW a clear
  `math domain error` on out-of-domain / non-finite inputs rather than emit silent NaN/inf. `polar`
  is the one entry point that reaches libc directly with unvalidated user input.
- fix idea: validate in the `polar` lambda before calling `std::polar` — throw a `math domain error`
  when `!std::isfinite(r) || !std::isfinite(theta) || r < 0`. Matches the analytic-set guards.

## RULED OUT (verified, correct or documented by-design — do NOT fix)
- **Scalar `+`/`-` on a Matrix throws** ("requires Matrices of equal shape"): docs
  (docs/pages/10-stdlib.md:208,556) explicitly scope scalar to `*` only ("matrix addition/subtraction,
  and matrix or scalar multiplication"). By design.
- **Reverse scalar `2 * m` throws** ("unsupported operand ... 'Matrix' ... 'Integer'"): docs
  explicitly require the scalar be the RIGHT operand (`A * 2`, not `2 * A`). By design (left-operand
  dispatch; Integer has no Matrix knowledge).
- **`5 + a` (real on the left of a Complex) throws**: docs say "Complex-on-the-left; reals coerce to
  the real axis". `a + 5` works. By design.
- **Complex is UNHASHABLE** (dict key / set member throws `unhashable type 'Complex'`): documented.
- **`Complex(2,0) == 2` and `2 == Complex(2,0)` both True; `Complex(1,0) == True` is False** — matches
  Kirito's `1 == True` being False (Bool distinct from Integer). Symmetric, consistent.
- **Complex ordering** `< <= > >=` and **sort** throw "complex numbers are not ordered". Correct.
- **Complex `/` by zero** throws; **`0j ** -1` / `0j ** 1j`** throw; **`0j ** 0 == 1`**. Correct.
- **Analytic-set domain poles throw**: `log(0)`, `log10(0)`, `atanh(±1)`, `atan(±i)` throw; while
  `sqrt(-1)=i`, `acosh(0)`, `asin(2)` remain valid (do NOT throw). Verified.
- **Determinant / inverse correctness**: `det[[1,2],[3,4]] = -2`, `det[[2,1,1],[1,3,2],[1,0,0]] = -1`,
  complex `det[[1+i,2],[i,1]] = 1-i`; `inv(m)*m ≈ I` via `.compare`. Singular matrix -> `inverse()`
  throws "matrix is singular", `determinant()` returns `0.0` (documented conservative behavior).
  inf/NaN element in det/inverse (real & complex) throws "matrix contains a non-finite value". Solid.
- **Square-only ops** (`determinant`/`inverse`/`trace`) throw on a non-square matrix (real & complex).
- **`m[i,j]` out-of-range** and negative index throw cleanly (real & complex); 1-index gives a row,
  2-index an element, else a clear arity error.
- **Dimension checks**: `+`/`-` require equal shape; `*` matrix-multiply checks inner dims; `dot`
  requires equal-length vectors; `cross` requires two 3-element vectors; `norm` = Frobenius/2-norm.
  Complex `dot` is the Hermitian inner product (`conj(a)·b`) — verified `[i,1].dot([i,1]) = 2`.
- **Factories** (`zeros`/`ones`/`identity`/`vector`, real & complex) reject negative dims and cap
  total elements at 16M (`Matrix too large`), overflow-safe (`c!=0 && r > kMax/c`).
- **`_setstate_` corruption guards** (Matrix/Complex/ComplexMatrix): reject negative dims, short
  state, element/shape size mismatch, oversized dims, and (complex) short `[re,im]` element pairs.
  serialize + dump round-trips verified via `.compare`. All solid — the deserialization attack
  surface is well-covered.
- **GC safety** in `apply` callbacks (both files): the per-element arg is `RootScope`-rooted across
  `fn.call`; the result matrix is a C++-local `unique_ptr` (not arena-visible, not sweepable). Safe.
- **Constructor input validation**: flat/ragged lists, string cells, huge dims all throw clean errors.

## DRY note
- `MatrixVal` and `ComplexMatrixVal` are near-identical twins (rows/cols/at/str/equals/getItem/setItem/
  getAttr method table/`_getstate_`/`_setstate_`/factories), differing only in `double` vs `cdouble`
  and the Hermitian dot / conjugate / hermitian additions. Large duplicated surface that could drift;
  a shared template base would DRY it. (Flagging for the DRY aggregator; not a correctness bug.)

## LOG
- Read both source files + tensor.hpp determinant/inverse/matmul/scalarOp fully.
- Probed: matrix scalar/matrix ops, det/inv/trace/transpose square checks, index bounds, singular
  detection, NaN/inf elements, 3x3 det, empty/0-dim matrices, factories, huge-dim guards.
- Probed: complex arithmetic, div-by-zero, ordering, pow singularities, full analytic domain-pole set,
  cross-type equality, unhashability, polar edge cases, is_zero threshold, modulus/argument/norm2.
- Probed: complex matrix transpose/conjugate/hermitian/det/inv/trace/dot(Hermitian)/cross/norm, dim
  mismatches, scalar mul.
- Probed: `_setstate_` corruption (all three types) + serialize/dump round-trips.
- Only confirmed defect: F1 (polar non-finite/negative input -> silent NaN, UB). Everything else
  ruled out as correct or documented by-design.
