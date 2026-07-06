# A10 — Builtins Audit (Kirito v1.14)

Area: builtin function surface (`installBuiltins` in `runtime.hpp`) + type constructors/converters
`Integer`/`Float`/`String`/`Bool`/`List`/`Set`/`Dict`/`Bytes`.

Status: **in progress**.

Method: static read-only scan of `src/`, plus throwaway `.ki` probes confirmed against
`build-debug/ki`. Prior findings (v1.13 `A11_builtins.md`, v1.12) merged — hunting NEW angles.

Builtin *functions* registered in `KiritoVM::installBuiltins()` (`runtime.hpp:2807+`). `builtins.hpp`
holds only the value classes + UTF-8/float helpers.

---

## Findings

