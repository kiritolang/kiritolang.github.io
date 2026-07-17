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

### F07-2 [Med] BigInt vs Float equality breaks transitivity + poisons Set/Dict
- stdlib_int.hpp:529-536 (BigIntVal::equals) handles BigInt/Integer/Bool but NOT Float. So
  `BigInt(3) == 3.0` is False even though `3 == 3.0` is True and `3 == BigInt(3)` is True.
  Equality is no longer transitive across the three numeric types.
- Trigger (CONFIRMED via ki):
  `3 == 3.0` → True, `3 == BigInt(3)` → True, but `3.0 == BigInt(3)` → False.
  `{3.0, BigInt(3), 3}` → len 2 (BigInt(3) coexists with 3.0 though BigInt(3)==3==3.0).
  `d[BigInt(3)]="big"; d.get(3.0)` → "missing" but `d.get(3)` → "big".
- Why it matters: BigInt hashes equal to an integral Float (both `std::hash<int64_t>(3)`), so they land
  in the same Set/Dict bucket but compare unequal — a Dict keyed by a BigInt is reachable via Integer
  but not the equal Float. Silent wrong lookups / duplicate set members. The CLAUDE.md contract
  ("hashes equal to an equal native Integer, shared Dict/Set bucket") is only half-honored: it forgot
  the Integer==Float bridge.
- Fix idea: in BigIntVal::equals add a Float branch — if the other Float is integral and in-range,
  compare exactly (BigInt-from-that-int64 cmp), matching intFloatEqual's exactness; else False.
  (FloatVal::equals need not change — kiEquals already reflects Instance-kind mismatches, calling
  ob.equals(oa).) Consider also whether `<`/`>` should work BigInt-vs-Float (currently throws).
- Test to add: `BigInt(3) == 3.0`, `3.0 == BigInt(3)`, `len({3, 3.0, BigInt(3)}) == 1`,
  `{BigInt(3): 1}.get(3.0) == 1`.
- Verified-real: CONFIRMED.

### F07-3 [Med] Float `%` (and the mod path) returns NaN for a finite dividend with an infinite divisor
- runtime.hpp:281-284 — float Mod computes `x - std::floor(x / y) * y`. For `y = inf`,
  `floor(x/y) = 0.0`, and `0.0 * inf = NaN`, so the result is NaN even though the mathematically
  correct floor-mod is `x` (Python `5.0 % inf` == `5.0`).
- Trigger (CONFIRMED via ki, vs Python): `5.0 % inf` → Kirito `nan`, Python `5.0`;
  `(-5.0) % inf` → Kirito `nan`, Python `inf`; `5.0 % (-inf)` → Kirito `nan`, Python `-inf`.
  (`5.0 // inf` → `0.0` matches.)
- Why it matters: a wrong numeric result (NaN) where a clean value exists, and it contradicts the
  module-wide "don't return silent NaN rubbish" ethos. Exotic inputs, but silently wrong.
- Fix idea: special-case an infinite divisor with a finite dividend (return x when signs align per
  floor semantics), or use `std::fmod` then apply the floor sign-correction (which handles inf), e.g.
  `r = std::fmod(x,y); if (r != 0 && (r<0)!=(y<0)) r += y;` — std::fmod(5,inf)=5.
- Test to add: `5.0 % inf == 5.0`, `(-5.0) % inf == inf`.
- Verified-real: CONFIRMED (reproduced, diverges from Python floor-mod).

### F07-4 [Low] BigInt vs Float ordering/arithmetic throws a misleading "arithmetic" message
- stdlib_int.hpp:594-596 — BigIntVal::binary coerces rhs unconditionally via `coerce(...,"BigInt
  arithmetic")`, so a *comparison* like `BigInt(3) < 3.5` throws "BigInt arithmetic expects a BigInt
  or Integer" (says "arithmetic" for a comparison) rather than either comparing or a clear message.
- Trigger (CONFIRMED): `BigInt(3) < 3.5` → throws "BigInt arithmetic expects a BigInt or Integer".
- Why it matters: minor UX/consistency — native `3 < 3.5` works; BigInt cannot be ordered against a
  Float at all, and the diagnostic misdescribes the operation.
- Fix idea: for Lt/Le/Gt/Ge/Eq/Ne give a comparison-specific message, or support Float comparison via
  an exact BigInt-vs-Float compare (ties into F07-2).
- Verified-real: CONFIRMED.

