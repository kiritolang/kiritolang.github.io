# A10 — String (Unicode/code-point) + Bytes Audit

Area: String value type + all methods (code-point indexing/slicing/iteration, case, strip/split/join/replace, search, predicates, pad, partition, levenshtein, format mini-spec, f-string spec, str-vs-repr, encode/decode) and Bytes value type + all methods (immutable 0-255, indexing→Integer, slicing→Bytes, iteration→Integers, +/*, ordering, hashable, hex/fromhex, Bytes(x[,enc]), serialization).

Sources: `src/kirito/bytes.hpp`, String/Bytes impls in `src/kirito/runtime.hpp` (+ wherever else).
Tests: `tools/tests/unit/test_bytes.cpp`, `test_strbytes_deep.cpp`, `test_unicode.cpp`, `tools/tests/scripts/probe_unicode_conformance.ki`.

Status: IN PROGRESS.

---

## Findings

