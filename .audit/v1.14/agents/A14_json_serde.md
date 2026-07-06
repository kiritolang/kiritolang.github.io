# A14 — json + serde audit (IN PROGRESS)

Scope: `src/kirito/stdlib_json.hpp`, `src/kirito/stdlib_serde.hpp` (shared `flatten`/`rebuild`),
`src/kirito/stdlib_serialize.hpp` (text `serialize`), `src/kirito/stdlib_dump.hpp` (binary `dump`).
Method: READ-ONLY on src; `.ki` probes confirmed with `build-debug/ki`.

## Surface enumerated
- json: `parse`/`loads`, `stringify`/`dumps(value, indent=0)`. Lenient number grammar, \u + surrogate
  pairs, U+FFFD for lone surrogates, NaN/Infinity/-Infinity accepted+emitted. Depth guard 1000.
- serde::flatten/rebuild: Node table, identity ids, cycles, 4-pass rebuild (content-hashed members
  deferred to pass 4). Instance→_getstate_ (Stateful) or attrs (Object). Native opt-ins: Bytes,
  Matrix/Vector, Complex/ComplexMatrix, Tensor(grad→throw), DateTime, Random, parallel prims.
- serialize (text KSER1) + dump (binary KDMP v1) share the core; differ only in codec.

(findings below)
