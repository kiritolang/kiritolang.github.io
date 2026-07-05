# A11 — Builtins Audit (Kirito v1.13)

Area: builtins (`range`, `sum`, `min`, `max`, `abs`, `round`, `sorted`, `enumerate`, `zip`, `map`,
`filter`, `len`, `type`, `id`, `import`, `inspect`, `all`, `any`, `reversed`, `divmod`, `isinstance`,
`hasattr`, `ord`, `chr`, `bin`, `oct`, `hex`, `pow`, `bitand`/`bitor`/`bitxor`/`bitnot`, `shl`/`shr`,
`format`, and the `Integer`/`Float`/`String`/`Bool`/`List`/`Set`/`Dict`/`Bytes` constructors).

Method: static read-only analysis. No builds run. Findings below; confirmed bugs first, then
weak-spots, then coverage gaps, then DRY.

Note: `builtins.hpp` itself holds only the value classes (`NoneVal`/`BoolVal`/`StrVal`/`IntVal`/
`FloatVal`) and UTF-8/float-format helpers. The builtin *functions* are registered in another file —
locating & reading it below.

---

