# Coverage findings — `int` (BigInt) and `complex` stdlib modules

Test: `tests/scripts/cov_int_complex.ki` (302 assertions, exit 0, prints `cov int/complex: all passed`).
Behavior verified against `./build-release/ki`. No bugs or silent-errors found — every documented
should-error case throws the correct diagnostic. Deltas below are doc-vs-impl nuances worth recording,
not defects.

## Bugs / silent errors

None. All hardening cases (parse errors, division/modulo by zero, modpow modulus 0, negative
exponents, toint overflow, analytic singularities, unordered comparisons, non-finite polar, ragged
matrices, singular inverse, non-square det/trace/inverse) raise the correct error-message substring.
toint on a value beyond int64 throws rather than silently wrapping.

## Doc-vs-impl deltas (intentional, documented in source comments)

1. **BigInt vs huge-Float equality is a documented false-negative.** The docs' F07-2 contract shows
   `int.big(3) == 3.0` (True) and dedup in Set/Dict. The impl (`stdlib_int.hpp` `equals`) only compares
   a Float exactly when it lies in the int64 range; a Float outside `[-2^63, 2^63)` is treated as
   unequal even when it is the exact value. Confirmed:
   - `int.pow(2, 64) == 18446744073709551616.0` → `False` (the exact double of 2^64, yet unequal).
   - `int.pow(2, 62) == 4611686018427387904.0` → `True` (in range, exact).
   This is an accepted rare false-negative called out in the source comment (a genuinely-equal huge
   power-of-two float), not a bug. The docs do not mention the int64-range caveat.

2. **`Complex`/`ComplexMatrix.compare(...)` with the default `abs_tol = 0.0` reports False for a
   near-zero value compared against exact zero.** With `abs_tol = 0.0`, `cClose(1e-16, 0.0)` fails
   because `rel_tol * max(|a|,|b|)` is ~0 there. So `(m2 * m2.inverse()).compare(complex.identity(2))`
   needs an explicit `abs_tol` (e.g. `1e-9`) to pass on the near-zero off-diagonal residues. This is
   the standard rel/abs-tolerance semantics (matches `math.isclose`), but the docs' `compare` entry does
   not warn that comparisons against zero require a nonzero `abs_tol`.

3. **`i ** 2`, `(1+i)**2`, `polar(1, π/2)` carry a tiny floating residue.** Complex `**`/`pow` go
   through `std::pow`, so `i**2` is `-1.0 + 1.22e-16i` (not a clean `-1.0+0.0i`), and `(1+i)**2` is
   `1.22e-16 + 2.0i`. Exact `==` therefore fails; `.compare()` is required (as the docs state). Recorded
   because the string form is not the mathematically-clean value.

## Behavioral notes (not deltas, just pinned by the test)

- Reflected-operator rule holds for both types: `3 + int.big(2)` and `2 + complex.Complex(1,1)` (and
  even `0 + complex.i`) throw `unsupported operand type '<T>' for arithmetic with 'Integer'`. There is
  no special-case for `Integer(0)` — the BigInt/Complex must be the left operand.
- BigInt `/` yields a Float; `//` yields a BigInt; floor semantics for negatives match native Integer.
- `int.big(True/False)` coerces to 1/0; `int.big(1) == True` is True, `int.big(3) == True` is False.
- `fromstring(s, base)` does NOT accept a `0x`/`0o`/`0b` prefix (prefix auto-detect is only `big`/
  `BigInt` in base 0); `int.fromstring("0xff", 16)` throws `invalid integer literal`.
- Negative `**` exponent on a BigInt returns a Float (`int.big(2)**-1 == 0.5`); on zero base it throws
  `zero cannot be raised to a negative power`.
- ComplexMatrix stringification collapses `-0.0` on the imaginary part to `+0.0`
  (`conj(3+0i)` prints `3.0+0.0i`, not `3.0-0.0i`).
- `is_zero()` is tolerant (|z| < 1e-10): `Complex(1e-11,0).is_zero()` is True, `Complex(1e-9,0)` False.
