# A10 — serde: json / serialize / dump (v1.16.1)

Audited directly via adversarial round-trips (permanent golden `tests/scripts/spec_v1161_serde.ki`).
Covered: json scalars/nested/unicode-escape/malformed-throws + INSERTION-ORDER preservation; serialize
(text) shared-ref aliasing + self-referential cycle + inf/-inf; dump (binary) Bytes output + dict/set
round-trip + user-class-by-attributes; and critically the v1.16.0 **ordered Dict/Set preserving
insertion order through BOTH serialize and dump** (validates the A2 dense-collection change end-to-end
in serde).

## Result: NO bugs found. Ordered-collection serde round-trip is correct and deterministic.

## Coverage notes
- `_getstate_`/`_setstate_` custom protocol + a native value type opting in (Matrix/Complex/DateTime/
  Random/Tensor) round-trip — covered by older serde specs, not re-exercised adversarially here.
- The "loads EXECUTES code" trust boundary (a Function/class blob re-parses its source) — a security
  property, tested elsewhere; not a correctness gap.
- Deeply nested plain-data hitting the 8000-frame depth guard — covered by an existing deep test.
- json surrogate-pair decoding + json indent pretty-print — partially covered; worth an explicit case.
