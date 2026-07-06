# A10 — Builtins Audit (Kirito v1.14)

Area: builtin function surface (`installBuiltins` in `runtime.hpp`) + type constructors/converters
`Integer`/`Float`/`String`/`Bool`/`List`/`Set`/`Dict`/`Bytes`.

Status: **complete**.

Method: static read-only scan of `src/`, plus throwaway `.ki` probes confirmed against
`build-debug/ki`. Prior findings (v1.13 `A11_builtins.md`, v1.12) merged — hunting NEW angles.

Builtin *functions* registered in `KiritoVM::installBuiltins()` (`runtime.hpp:2807+`). `builtins.hpp`
holds only the value classes + UTF-8/float helpers.

---

## Overall assessment

This subsystem is **exhaustively tested and robust**. Every fresh angle I probed against
`build-debug/ki` behaved correctly and, for almost all, already had a golden `.ki` or C++ test:
`Integer("0xFF")`/`Integer("  12  ")`/`Integer(-3.9)`(→-3, trunc-toward-zero)/`Integer("")`(throw)/
`Integer(inf)`(throw); `Float("inf")`/`Float("nan")`/`Float("0x1p4")`(throw); `Integer(True)`/`Float(True)`;
`round(0.5/2.5/3.5/-2.5)` (half-away-from-zero, not banker's — pinned in r4/r6/r7/r9/test_audit_v113);
`round(2.675,2)==2.67`, `round(12345.0,-2)==12300.0`; `range` float-arg/zero-step/huge (all throw/guard);
`sum` start (pos+kw), empty, INT64 wrap (→INT64_MIN); `min/max` key/default/empty/mixed-uncomparable;
`zip` unequal/empty; `enumerate` empty; `divmod` floats/negatives/`INT64_MIN,-1`; `pow` neg-base-frac
(throw)/modular neg-exp(throw)/mod=1(→0); `abs`(INT64_MIN wrap)/Bool-reject; `bin/oct/hex` of
zero/negative (sign-magnitude `-0xff`); `bitand/bitor/bitxor/bitnot/shl/shr` on negatives + huge/neg
shift counts; `ord`(multi-char/empty throw)/`chr`(neg/>0x10FFFF/surrogate throw); `filter/all/any`
`_bool_` dispatch; multi-key `sorted`; `List/Set/Dict` from Dict/Set sources; `Bytes` from
List/Integer/String + out-of-range element (throw); `isinstance` type-name String; `hasattr` on a
None-valued attr (True) and on a class value (False).

Two prior-cycle findings are now RESOLVED and one gap CLOSED (see "Resolved / closed" below). Net-new
material this cycle is limited to one small coverage/design gap plus three still-open carry-forwards.

## Findings

