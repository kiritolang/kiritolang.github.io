# Doc-vs-impl audit: `matrix` and `complex` stdlib modules

Verified `docs/pages/10-stdlib.md` (`## matrix`, `## complex`) against `src/kirito/stdlib_matrix.hpp`
and `src/kirito/stdlib_complex.hpp` with two exhaustive golden tests:

- `tools/tests/scripts/verify_matrix.ki` (+ `.expected` = `OK matrix\n`) — 72 assertions.
- `tools/tests/scripts/verify_complex.ki` (+ `.expected` = `OK complex\n`) — 114 assertions.

Both green under `build-debug/ki`. No `src/` changes.

## Discrepancies

**None.** Every documented function, method, attribute, operator, constant, and error mode behaves as
the docs describe. Doc wording that could look like a mismatch but is consistent:

- `matrix` inverse of a singular matrix: doc says it throws `"singular"`; the actual message is
  `matrix is singular (no inverse)` — the doc's quoted word is a substring, so it is accurate. Same
  message for `ComplexMatrix.inverse()` of a singular matrix.
- `complex` `is_zero()`: doc says "magnitude below `1e-10`"; impl tests `std::norm(z) < 1e-20`
  (squared magnitude), i.e. magnitude `< 1e-10`. Consistent.

## Pinned current behaviour (exact error messages / semantics)

### matrix (stdlib_matrix.hpp)
- Construction: `Matrix(nested list)`; `Matrix(rows, cols)` / `Matrix(rows=, cols=)` -> zero matrix.
  Factories `zeros`/`ones`/`identity`/`vector`. Elements are `Float`; `m[i]` is a `List` of `Float`.
- `==` is EXACT (same shape + bit-equal elements; NaN never equal). `.compare(other, rel_tol=1e-9,
  abs_tol=0.0)` is tolerant; shape mismatch -> `False`.
- `determinant` of a singular matrix returns `0.0` (conservative ~1e-15 pivot guard); `inverse` of a
  singular matrix THROWS (message contains `singular`).
- Scalar multiply requires the scalar on the RIGHT (`A * 2`); `2 * A` raises
  `unsupported operand type 'Matrix' for arithmetic with 'Integer'` (dispatched by the Integer, not
  silent).
- Exact error strings:
  - `determinant requires a square Matrix` / `inverse requires a square Matrix` /
    `trace requires a square Matrix`
  - `Matrix +/- requires Matrices of equal shape`
  - `Matrix multiply: inner dimensions differ`
  - `Matrix index out of range` (element OOB and negative index) / `Matrix row index out of range`
  - `Matrix index needs 1 (row) or 2 (element) indices`
  - `Matrix element assignment needs two indices: m[i, j] = v`
  - `Matrix rows must have equal length` (ragged ctor)
  - `Matrix element expected a number, got '<Type>'` (non-number cell)
  - `dot requires vectors of equal length` / `dot requires vectors (a 1×n or n×1 Matrix)` /
    `dot expects a Matrix vector`
  - `cross is only defined for two 3-element vectors`
  - `compare expects a Matrix`

### complex (stdlib_complex.hpp)
- `Complex(re[, im=0])`, `of(re, im)`, `real(re)`, `polar(r, theta)`; constants `i`/`zero`/`one`.
  Reals coerce to the real axis; `Complex` must be the LEFT operand when mixing with a number.
- `==` EXACT (real+imag bit-equal); `Complex(2, 0) == 2` (and symmetric: `2 == Complex(2,0)` True).
  `.compare(other, rel_tol=1e-9, abs_tol=0.0)` tolerant.
- `is_zero()` tolerant: `std::norm(z) < 1e-20` (magnitude < 1e-10).
- Analytic set defined across the plane (`sqrt(-1)=i`, `log(-1)=iπ`, `asin(2)`, `acosh(0)` valid, do
  NOT throw). `0 ** 0 == 1`. Hermitian `dot` (`Σ conj(uᵢ)·vᵢ`) -> real for `v.dot(v)`.
- Exact error strings:
  - `complex numbers are not ordered (no <, <=, >, >=)` (all of `< <= > >=`)
  - `complex division by zero`
  - `<fnname>: math domain error (logarithm of zero)` (log/log10 of 0)
  - `atanh: math domain error (atanh of ±1)`
  - `complex pow: zero to a negative or complex power` (`zero ** -1`, `pow(zero, i)`)
  - `Complex arithmetic expects a Complex or a number` (bad RHS type)
  - ComplexMatrix: `determinant requires a square ComplexMatrix` /
    `inverse requires a square ComplexMatrix` / `trace requires a square ComplexMatrix`;
    `ComplexMatrix +/- requires matrices of equal shape`;
    `ComplexMatrix multiply: inner dimensions differ`;
    `ComplexMatrix index out of range` / `ComplexMatrix row index out of range`;
    inverse of singular -> message contains `singular`;
    `Matrix rows must have equal length` (ragged ctor);
    dot/cross messages mirror the real Matrix (`dot requires vectors of equal length`,
    `dot requires vectors (a 1×n or n×1 ComplexMatrix)`, `cross is only defined for two 3-element vectors`).
