# A17 — json + serialize (text) + dump (binary) + shared serde core

Audit of untrusted-input parsers: `src/kirito/stdlib_json.hpp`, `stdlib_serde.hpp`,
`stdlib_serialize.hpp`, `stdlib_dump.hpp`. READ-ONLY, static reasoning. Security/robustness focus.

Status: COMPLETE.

## Surface enumerated

- **json** (`stdlib_json.hpp`): `parse`/`loads` (recursive-descent, DepthGuard=1000; objects→Dict,
  arrays→List, `\u`+surrogate handling→U+FFFD substitution for unpaired, NaN/Infinity/-Infinity,
  lenient number grammar, int→float widening on overflow→inf); `stringify`/`dumps` (compact + indented,
  cycle set `active` + size>1000 guard, control-char `\u` escaping, shortest-roundtrip floats).
- **serde core** (`stdlib_serde.hpp`): `flatten` (recursive std::function, depth guard 10000/1500-asan,
  identity ids, cycle reservation) + `rebuild` (3-pass ITERATIVE, all ids bounds-checked via `checkId`,
  root id checked). Tags None/Bool/Integer/Float/String/List/Dict/Set/Object(user attrs)/Stateful
  (_getstate_/_setstate_). Bytes = NativeClass(Instance-kind) → Stateful path via getAttr's
  _getstate_/_setstate_ + registerDeserializer("Bytes").
- **serialize** (`stdlib_serialize.hpp`): text codec "KSER1 …", `TextReader` with countToken bounded by
  blob length, length-prefixed strings/names, all stol/stod wrapped→clean error in `loads`.
- **dump** (`stdlib_dump.hpp`): binary "KDMP" v1 LE, `Reader::need` bounds-checks every read,
  count>data.size() rejected, reserve capped 65536, dumps→Bytes.

## Positives (security concerns checked and OK)

- Recursion: JSON parse guarded (1000); serde flatten guarded (10000/1500); JSON write guarded (1000);
  rebuild is iterative; **GC mark is iterative** (worklist, vm.hpp:143) — no stack overflow from a deep
  deserialized structure at next GC; equals/stringify have their own depth guards.
- Truncation: binary `Reader::need` + text `token()`/`rawBytes` throw cleanly at every offset
  (probe_serde_truncation.ki fuzzes this).
- Integer overflow in counts: text `countToken` bounds to `s_.size()`; binary Dict loop uses
  `uint64_t(c)*2`; link counts never pre-reserved (bounded by real stream length).
- ref-index OOB: `checkId` + root check in rebuild throw "…id out of range".
- GC during flatten: `_getstate_` results + synthesized keys held in `RootScope`; graph objects stay
  reachable via the rooted root, so `ids` raw `Object*` keys don't dangle.
- Resource-like natives (Socket/Session/File/Regex): no _getstate_ and no registered deserializer →
  flatten hits the Instance "define _getstate_/_setstate_" throw. Correctly refuse.

---
