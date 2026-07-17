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

### F07-5 [Low] Float("1e400") throws on overflow, but the literal 1e400 and Float("inf") yield inf
- runtime.hpp:3460-3475 — the Float() string converter's `catch (...)` swallows parseDouble's
  `std::out_of_range` (overflow) and rethrows "cannot convert String to Float". But the float LITERAL
  path (parser.hpp:865) catches the same out_of_range and returns HUGE_VAL (inf), and `Float("inf")`
  returns inf. So `1e400` (source) → inf, `Float("inf")` → inf, but `Float("1e400")` → error.
- Trigger (CONFIRMED): `Float("1e400")` throws; `1e400` prints `inf`; Python `float("1e400")` == inf.
- Why it matters: minor conformance/consistency — a round-trip `Float(String(1e400))` fails though the
  value is representable as inf. (Parallel: `Integer("99999999999999999999")` throws while the literal
  wraps — but for Integer, throwing on overflow is defensible; the Float→inf case is the clearer gap.)
- Fix idea: in the Float() catch, distinguish `std::out_of_range` → return `copysign(HUGE_VAL, sign)`
  matching the literal path; only invalid_argument/trailing → the "cannot convert" error.
- Test to add: `Float("1e400") == math.inf`, `Float("-1e400") == -math.inf`.
- Verified-real: CONFIRMED.

### F07-6 [Low] int.modpow/BigInt.modpow accept a negative modulus; native pow(b,e,m) rejects it
- stdlib_int.hpp:341-342 (modpow) only checks `mod.isZero()`, so a negative modulus produces a
  floor-mod residue (`int.modpow(2,10,-1000)` → -976). The native 3-arg `pow(b,e,m)` (runtime.hpp:3962)
  explicitly throws "pow modulus must be positive".
- Trigger (CONFIRMED): `int.modpow(2,10,-1000)` → -976; `pow(2,10,-1000)` → throws.
- Why it matters: two modular-exponentiation entry points with opposite negative-modulus policy. The
  BigInt result (Python-style floor mod, negative) is arguably fine, but the inconsistency is a
  surprise; docs scope modpow to non-negative under `pow`.
- Fix idea: pick one policy — either allow negative modulus in native pow (floor mod, like BigInt) or
  reject it in modpow — and document it.
- Verified-real: CONFIRMED.

### F07-7 [Low] len({nan, nan}) == 2 for the same nan object; a nan Dict key is unrecoverable
- runtime.hpp:124-130 / set insertion — NaN != NaN (exact IEEE, by design), and Set/Dict insertion has
  no identity short-circuit, so inserting the SAME nan object twice yields two set elements, and
  `d[nan]=1; d.get(nan)` returns the default (never finds it).
- Trigger (CONFIRMED): `len({m.nan, m.nan})` → 2 (same module-level nan handle both times);
  `d[m.nan]=1; d.get(m.nan,"missing")` → "missing". Python (identity fallback) gives len 1 / retrievable.
- Why it matters: a consequence of the documented exact-== contract, but it means a nan is a write-only
  Set/Dict key and the same object de-dups to 2. Likely INTENDED (confirm with maintainer); worth a
  documented test either way. Python-parity would need an `is`-fallback in bucket compare.
- Verified-real: CONFIRMED (flagging as design-confirm + coverage, not asserting it is wrong).

### F07-8 [High] min/max/sorted (kiLessThan) can't order BigInt (or any NativeClass) though `<` works
- runtime.hpp:456-497 (kiLessThan) — the ordering used by min/max/sorted/reversed-key/List compare
  handles numeric, String, Bytes, List, and `InstanceValue` with `_lt_`, then throws "cannot order".
  A NativeClass value (BigInt, and also DateTime/Matrix/etc.) has `kind()==Instance` but is NOT an
  `InstanceValue`, so the `dynamic_cast<const InstanceValue*>(&x)` at line 495 fails and it throws —
  even though BigInt fully supports `<`/`<=`/`>`/`>=` via its `binary(BinOp::Lt)`.
- Trigger (CONFIRMED via ki): `BigInt(3) < BigInt(5)` works, but
  `min(BigInt(5), BigInt(3))` and `sorted([BigInt(5), BigInt(3), BigInt(9)])` both throw
  "cannot order 'BigInt' and 'BigInt'". No clean workaround (key= can't map a huge BigInt to an
  orderable scalar without losing magnitude).
- Why it matters: BigInt is a headline value type documented with full ordering + floor //,% "matching
  native"; sorting a list of big integers is an obvious operation that silently fails. The same gap
  hits every ordered NativeClass.
- Fix idea: after the InstanceValue+_lt_ branch, for any `kind()==Instance` fall through to
  `x.binary(vm, BinOp::Lt, a, b)` (let the native type's own binary throw if genuinely unordered, e.g.
  Complex). Mind reflected ordering: `3 < BigInt(5)` (Integer on left) still won't dispatch — either
  document that or special-case numeric-vs-native.
- Test to add: `sorted([BigInt(5), BigInt(1), BigInt(3)])` and `min`/`max` over BigInts.
- Verified-real: CONFIRMED.

### F07-9 [Med] sum() (and min/max) reject BigInt and Complex though `+` / operators work
- runtime.hpp:3665-3683 (sum) accumulates only Integer/Float in a native int64/double path; any other
  element throws "sum expects numbers", and a non-Integer/Float `start` throws "sum start must be a
  number". So `sum([BigInt...])` and `sum([Complex...])` both fail — summing big integers is the
  canonical BigInt use case.
- Trigger (CONFIRMED): `sum([BigInt(5), BigInt(3)])` → "sum expects numbers";
  `sum([Complex(1,1), Complex(2,2)])` → "sum expects numbers"; even with a matching `start` it throws
  "sum start must be a number". (Same root theme as F07-8 for min/max/sorted.)
- Why it matters: BigInt/Complex are documented first-class numerics with full `+`; the aggregate
  builtins silently reject them, so the obvious `sum(big_numbers)` fails with no clean workaround
  besides a manual fold.
- Fix idea: when an element (or start) is not Integer/Float, fall back to `applyBinaryOp(BinOp::Add,
  acc, elem)` starting from `start` (default 0) — mirrors Python's type-generic sum and reuses the
  operator semantics; keep the int64/double fast path for the pure-scalar case.
- Test to add: `sum([BigInt(5), BigInt(3)]) == BigInt(8)`; complex sum with `start=complex.zero`.
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

