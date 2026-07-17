# A07 — Numerics audit (Integer / Float / Complex / BigInt + math)

Scope: object.hpp + runtime.hpp numeric paths, stdlib_math.hpp, stdlib_complex.hpp,
stdlib_int.hpp (BigInt), numeric literals in lexer.hpp.

Findings below (append-only).

### F07-1 [Med] math.trunc returns Float while math.floor/ceil return Integer (inconsistent)
- stdlib_math.hpp:83 — `unary("trunc", std::trunc)` registers trunc in the Float-returning `unary`
  batch, so `math.trunc(3.7)` yields `3.0` (Float). But `floor` (line 171) and `ceil` (line 174) are
  defined separately returning Integer via `toInt64Checked`. Python's `math.trunc` returns int, and
  the three are grouped as one family in CLAUDE.md ("floor/ceil/trunc").
- Trigger: `math.trunc(3.7)` → `3.0` (Float); `math.floor(3.7)` → `3` (Integer). CONFIRMED via ki.
- Why it matters: surprising asymmetry — code that does `xs[math.trunc(i)]` gets a Float index error,
  or `math.trunc(x)` used where an Integer is expected. Also no overflow guard on trunc (returns inf
  as Float for huge input, whereas floor/ceil throw "out of Integer range").
- Fix idea: define trunc like floor/ceil: `m.fn("trunc", ..., "Integer", toInt64Checked(std::trunc(x),"trunc"))`.
- Test to add: assert `type(math.trunc(3.7)) == Integer` and it matches `math.floor` for positives,
  `math.ceil` for negatives; `math.trunc(1e300)` throws.
- Verified-real: CONFIRMED (behaviour reproduced).

