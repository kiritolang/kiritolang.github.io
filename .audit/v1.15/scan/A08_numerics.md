# A08 Numerics

Status: COMPLETE

Scope: Integer/Float ops (runtime.hpp numericBinary/applyBinaryOp), builtins (abs/round/divmod/pow/min/max/sum/bin/oct/hex/ord/chr/bitand/bitor/bitxor/bitnot/shl/shr), math module (stdlib_math.hpp), int module BigInt (stdlib_int.hpp).

Read so far: stdlib_int.hpp (full), stdlib_math.hpp (full), runtime.hpp numericBinary/applyBinaryOp/IntVal/FloatVal (full), builtins section (abs/round/divmod/pow[2&3]/min/max/sum/bin/oct/hex/ord/chr/bitand/bitor/bitxor/bitnot/shl/shr), test_eval_arith.cpp, test_domain_guards.cpp, test_bigint.cpp (full).

Overall impression: this subsystem is exceptionally well-hardened by prior rounds — INT64_MIN wraparound
is handled correctly and consistently everywhere I checked (unary neg, abs, //, %, shl/shr, bin/oct/hex
magnitude computation, range() count arithmetic), math domain errors are thorough and already regression-
tested (test_domain_guards.cpp), and test_bigint.cpp already pins Carmichael numbers, modinv non-coprime,
reflected-operator asymmetry, and hash/== cross-type agreement. Verified live against build-debug/ki
rather than relying on reading alone. Found one genuine, untested inconsistency (A08-1) in BigInt's `**`
operator; everything else adversarial I tried came back correct.

## A08-1: `BigInt(0) ** negativeExponent` silently returns `inf` instead of throwing  [MEDIUM] [HIGH]
- **Location**: src/kirito/stdlib_int.hpp:565-577 (`bigint::powOp`), used by `BigIntVal::binary` case
  `BinOp::Pow` at line 604.
- **What**: Every other numeric power path in Kirito throws "zero cannot be raised to a negative power"
  for a zero base with a negative exponent: native `Integer ** negative` (runtime.hpp:216-221, checks
  `x == 0` before falling back to Float), native `Float ** negative` (runtime.hpp:254-262, checks
  `x == 0.0 && y < 0.0`), and `math.pow(0, -1)` (stdlib_math.hpp:143-153, same check, already regression-
  tested in test_domain_guards.cpp:47). `bigint::powOp`'s negative-exponent branch is the ONLY one that
  skips this check:
  ```cpp
  inline Handle powOp(KiritoVM& vm, const Big& base, const Big& exp) {
      if (exp.neg) return vm.makeFloat(std::pow(toDouble(base), toDouble(exp)));   // Float, like Integer**negInt
      ...
  ```
  The comment explicitly claims it mimics `Integer**negInt`, but it doesn't reproduce that path's zero-
  base guard, so `std::pow(0.0, negative)` silently returns `+inf` (IEEE-754 pole-error behavior) instead
  of throwing. Note: `int.pow(base, exp)` (the MODULE function, stdlib_int.hpp:748-756) is unaffected —
  it unconditionally rejects a negative exponent regardless of base — so only the `**` OPERATOR on a
  BigInt value is inconsistent.
- **Repro** (confirmed against build-debug/ki):
  ```
  var io = import("io")
  var int = import("int")
  io.print(int.BigInt(0) ** (0-1))   # prints "inf" — should throw
  io.print(0 ** (0-1))                # throws "zero cannot be raised to a negative power"
  io.print(0.0 ** (0-1))              # throws "zero cannot be raised to a negative power"
  ```
  Confirmed non-zero base is fine (`int.BigInt(-2) ** (0-1)` == `-0.5`, matching native `(-2)**(-1)`), so
  the defect is specifically the zero-base case.
- **Proposed fix**: in `powOp`, before the `std::pow` call, check `if (base.isZero()) throw KiritoError("zero cannot be raised to a negative power");` — mirroring the message the other two paths already use so error text stays consistent language-wide.
- **Proposed test**: add to test_bigint.cpp's adversarial section:
  `CHECK_THROWS(ev(vm, "int.BigInt(0) ** (0-1)"));` and a differential check that
  `int.BigInt(0) ** (0-1)` and `0 ** (0-1)` fail with the same message text.

## Coverage gaps (not bugs, just untested combinations worth adding)

- `int.modinv` with a negative `a` (e.g. `int.modinv(0-3, 7)`) — divmodFloor normalizes correctly per
  code reading but no test pins it; only positive-`a` non-coprime/modulus<2 cases are tested.
- `int.gcd`/`int.lcm` with a negative-BigInt argument (e.g. `int.gcd(int.BigInt(0-48), 60)`) — the
  `.neg = false` normalization at the top of `bigint::gcd` looks correct but isn't exercised.
- `int.randomprime` at the `bits > 1<<16` throw boundary, and `bits < 2` — code has the guard
  (stdlib_int.hpp:785-786) but no direct test.
- `int.isprobableprime(n, rounds=0)` / negative rounds — guarded (`rounds must be >= 1`) but untested
  for the module function specifically (the BigInt *method* `isprobableprime` has the same guard, also
  untested).
- `round(x, ndigits)` with `ndigits` an extreme value close to but not past the ±323 clamp boundary
  (e.g. 323, -323, 324, -324) to pin the exact cutoff.
- `sum`/`prod`/`comb`/`perm` mixed with `Bool` operands (Bool is accepted as Integer 0/1 elsewhere in
  the language, e.g. tabular numeric reductions) — not clear these builtins accept a Bool list element;
  worth a quick check but not verified as a bug here (ran out of scope/time to confirm either way).
- `divmod` with mixed Integer/Float operands (e.g. `divmod(7, 2.0)`) — falls through to numericBinary's
  Float branch (should work, `floor(x/y)` and residual), but not explicitly tested for the divmod
  builtin's List-pair packaging path.

## DRY check

`numericBinary` is confirmed as the single shared core for Integer/Float `+ - * / // % **` and
comparisons — both `IntVal::binary` and `FloatVal::binary` delegate to it, and `applyBinaryOp`'s fast
path calls it directly, so there is no duplicated arithmetic logic to drift. BigInt (`BigIntVal::binary`)
is a deliberately separate implementation (different representation) but mirrors the same operator
semantics (floor div/mod, `/` → Float, zero-divisor throws) except for the one `**` zero-base gap above —
that gap is exactly the kind of drift the "shared core" design is meant to prevent, which is why it's
flagged rather than dismissed as an unrelated one-off.

## Summary

Subsystem is in very good shape after 4 prior audit rounds. One genuine bug found and confirmed live:
**A08-1** — `BigInt(0) ** negativeExponent` returns `inf` instead of throwing "zero cannot be raised to
a negative power" (every other numeric power path in the language — native Integer, native Float,
`math.pow` — already guards this; only `bigint::powOp`'s `**`-operator path misses the zero-base check,
despite its own comment claiming to mirror `Integer**negInt`). MEDIUM severity (silent wrong-value
`inf` rather than a crash/UB), HIGH confidence (reproduced against build-debug/ki). A handful of
coverage gaps noted (untested but appear correct on code reading: modinv with negative `a`, gcd/lcm with
negative BigInt args, randomprime bit-count boundaries, isprobableprime rounds<1 for the module fn).
No overflow/UB bugs found in Integer wraparound, shift, abs, or BigInt long-division paths — all
adversarial inputs tried (INT64_MIN//-1, abs(INT64_MIN), shl by 64/-1, 0**0, 0.0**-1, (-8)**(1/3),
Carmichael numbers, huge factorial/pow guards) behaved correctly.

