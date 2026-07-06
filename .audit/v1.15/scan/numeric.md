# v1.15 audit — numeric core

Subsystem: Integer/Float arithmetic, numericBinary fast path, div/mod/pow, exact IEEE-754 ==, .compare.
Source: src/kirito/runtime.hpp (numericBinary L155, applyBinaryOp L2180, makeNumericCompare L301).

## LOG
- Read runtime.hpp L60-400. Key helpers: wadd/wsub/wmul (uint64 wrap), ifloordiv (b==-1 guard),
  imod (b==-1 -> 0), ipow (wmul loop), compareIntFloat (exact int/float order), numericBinary.
- Fast path applyBinaryOp L2189: if both numeric -> numericBinary (same func as virtual path via
  IntVal::binary/FloatVal::binary -> numericBinary). So fast and slow share numericBinary. Divergence
  only possible if applyBinaryOp reaches numericBinary for cases the virtual path wouldn't, or vice
  versa. Note: IntVal::binary special-cases Mul with sequence rhs (Integer*List etc). Fast path at
  L2189 requires BOTH numeric, so Integer*List never hits fast path -> goes virtual -> IntVal::binary
  handles seq. OK consistent.

## RULED OUT / BY-DESIGN
- INT64_MIN wraparound: -IMIN, IMIN//-1, IMIN%-1(=0), abs(IMIN)=IMIN, divmod(IMIN,-1)=[IMIN,0],
  IMAX+1, 2**63, 2**64=0 — all defined two's-complement wrap. Correct & documented (fixed int64).
- floor-div/mod signs across all 4 combos (int + float): result carries divisor sign. Correct.
- identity a==(a//b)*b+a%b holds for floats tested (7.5,2.1 etc).
- pow: 0**-1 throws, (-2)**0.5 throws, (-8)**(1/3) throws, int neg-exp -> Float, right-assoc
  2**3**2=512. pow(base,exp,mod) uses __int128, guards neg mod / zero mod / neg exp. Correct.
- exact IEEE-754 ==: 0.1+0.2!=0.3, NaN!=NaN, inf==inf, 0.0==-0.0, hashing agrees (close floats
  distinct Set keys; {0.0,-0.0} len 1; {1,1.0} len 1; 2^53+1 != float 2^53). All correct.
- .compare formula: boundary inclusive (<=), abs_tol path, NaN never close, inf==inf close,
  negative tol short-circuits on exact ==, Bool rejected ("expects a number"), rel_tol=Bool rejected.
- FAST PATH vs SLOW PATH: NO divergence possible. applyBinaryOp L2189 fast path calls numericBinary;
  IntVal/FloatVal::binary ALSO call numericBinary. Single BinaryOp opcode. Compiler does NOT
  constant-fold arithmetic (only switch-case keys). Confirmed identical.
- Bool is NOT numeric in Kirito: True+True THROWS, True==1 is False, {True,1} len 2. This is
  strong-typing by design (Bool distinct from Integer), contra the briefing's Python assumption.
  NOT a bug.
- divmod uses numericBinary for both q and r (rooted). Correct.
- Integer literal & Integer(str) full-width two's-complement: 0xFFFF..=−1, decimal 2^63 wraps to
  INT64_MIN (matches lexer). By design (comment L2894-2897).

## FINDINGS

### F1 [LOW] NaN stringifies as "-nan" (sign-bit leak) instead of canonical "nan"
- where: src/kirito/builtins.hpp:99 floatToString (`%.15g`) — sign bit of NaN passes through glibc's %g
- repro:
```
var inf = 1e308*10.0
var nan = inf - inf          # inf-inf yields a NEGATIVE-sign NaN on x86
io.print(nan)                # => -nan   (expected: nan)
io.print(-nan)               # => nan    (sign flips — nonsense for NaN)
io.print(String(nan))        # => -nan
io.print(0.0*inf)            # => -nan
io.print(inf % 1.0)          # => -nan
```
- actual: NaN prints as `-nan` or `nan` depending on its (meaningless) sign bit.
  expected: canonical `nan` regardless of sign, as Python/most languages do.
- impact: display/serialization inconsistency; `String(nan)` non-canonical; two NaNs print differently.
  Note json rejects nan separately (should confirm), but print/String/repr leak it.
- fix idea: in floatToString, special-case `std::isnan(d)` -> return "nan" (and keep "inf"/"-inf" for
  infinities, which ARE signed meaningfully). One-line guard before the snprintf.

### F2 [SUSPECT/LOW] Integer(str) silently wraps in [2^63, 2^64) but throws >= 2^64 — surprising cliff
- where: src/kirito/runtime.hpp:2898-2903
- repro:
```
Integer("9223372036854775808")     # => -9223372036854775808  (silent wrap to INT64_MIN)
Integer("18446744073709551615")    # => -1
Integer("99999999999999999999999") # THROW: cannot convert String to Integer
```
- actual: a decimal string just past INT64_MAX silently wraps negative; a bigger one throws.
- expected(?): consistency — either both wrap or both throw. Matches lexer's full-width two's-comp
  design (comment says intentional, mirrors hex(-1) round-trip), so likely BY DESIGN, but the
  silent decimal wrap is a scripting footgun and the >=2^64 cliff is inconsistent.
- fix idea: leave as-is (design) OR make plain-decimal overflow throw while keeping hex/oct/bin
  full-width. Flagging for triage, not asserting a bug.

## MORE RULED OUT
- Division/modulo by zero: all paths throw clean messages ("division by zero" for `/`; "integer/float
  division/modulo by zero" for `//`/`%`). Minor cosmetic: `/` message doesn't distinguish int/float.
- round(): half-away-from-zero (2.5->3, -2.5->-3, 0.5->1), round(2.675,2)==2.67 (long double), neg
  ndigits, out-of-range/NaN/inf throw. Correct.
- pow overflow to inf allowed (not a domain error, per docs); (-2.0)**3.0==-8.0 (integral float exp
  on neg base OK); ipow O(log exp) no hang on 2**(10**18).
- NaN in min/max/sort/contains: unordered semantics, `nan in [nan]` False, `[nan]==[nan]` False —
  all consistent with exact-== design.
- trichotomy int/float at 2^63 and 2^53 boundaries: <,==,> mutually exclusive & correct (compareIntFloat exact).
- json.dumps(nan)=="NaN", dumps(inf)=="Infinity" — json NORMALIZES the sign (capital canonical),
  confirming F1's `-nan` leak is confined to the DISPLAY path (print/String/floatToString), not json.

## SUMMARY
Numeric core is very solid. 1 confirmed LOW finding (F1: NaN "-nan" display leak), 1 suspect
(F2: silent decimal-string overflow wrap, likely by-design). No fast/slow-path divergence exists
(structurally impossible — both call numericBinary). No memory-safety or resource-exhaustion issues found.
