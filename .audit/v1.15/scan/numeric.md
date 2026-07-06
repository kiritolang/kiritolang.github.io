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

## FINDINGS
(none yet)
