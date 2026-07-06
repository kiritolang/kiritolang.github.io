# v1.15 audit — serde subsystem (json, serialize KSER1, dump KDMP, shared serde core)

Source: stdlib_json.hpp, stdlib_serialize.hpp, stdlib_dump.hpp, stdlib_serde.hpp
Probe: ./build-debug/ki

## LOG
- Read all four source files deeply (json, serde core, dump, serialize).
- json.parse adversarial inputs (deep nest, trailing commas, unclosed, bad escapes, surrogates,
  dup keys, leading zeros, NaN/Inf/-Inf, huge int/float, `.5`/`1e`/`--5`/bare `-`, ctrl/nul in
  strings): ALL throw cleanly or parse per documented leniency. Deep-nest guard (1000) fires. No crash.
- json.stringify: cycles throw ("cannot serialize a cyclic structure"), Set/Function throw,
  deep-nest guard (active.size()>1000) fires, indent>100 throws, negative indent treated as 0,
  float round-trips exactly (shortest-roundtrip), ctrl chars escaped. Solid.
- serialize + dump: shared refs preserved (mutate-one-sees-other confirmed for both text+binary),
  cycles reconstruct (self-ref confirmed), user class by attrs + _getstate_/_setstate_ round-trip,
  class-not-in-registry on load throws cleanly, _setstate_ that throws propagates cleanly.
- Native value types round-trip EXACTLY: Matrix, Complex, DateTime, Random (stream continues),
  grad-free Tensor. Content-hashed members (DateTime/Bytes Set elements + Dict keys, instance
  keys with _hash_/_eq_) bucket correctly after rebuild (membership + lookup confirmed).
- Resource-like natives REFUSE serialization: Socket, BytesIO, Regex, Match, File all throw
  "cannot dump/serialize type ... (define _getstate_/_setstate_)". Good.
- Adversarial BINARY blobs (manufactured in Python, byte-forged): bad magic, bad version, truncated,
  root id OOB, forged huge object count, forged huge link count, link id OOB, forged huge string len,
  bad tag, forged huge dict pairs, missing rootId, empty, header-only, stateful-unknown-class — ALL
  throw cleanly (no crash/OOM/OOB). Guards: `n > data.size()` count check, `checkId` bounds, Reader
  `need()` truncation check, capped reserve(64K).
- Adversarial TEXT (KSER1) blobs: bad header, neg/huge count, bad tag, root OOB, malformed number,
  truncated string, empty, id OOB, negative id — ALL throw cleanly. countToken bounds counts to blob
  size; loads() translates std::exception (stol/stod) into a clean Kirito error.
- flatten depth guard (10000, 1500 under sanitizer) fires on deep nesting → throws not crashes.
  rebuild is iterative (two-pass, no recursion) → no stack overflow on deep/forged blobs.
- GC-safety review: GC only triggers at vm.alloc points (vm.hpp:59); container growth (std::vector/
  map) doesn't go through alloc, so no dangling-handle window between make*() and use in json parse /
  rebuild. flatten's `ids` map only holds root-reachable or roots-pinned Object*, so no freed-address
  reuse. Verified empirically: 300k-entry large-int JSON object parses with full integrity.
- Float exactness: 0.1+0.2, DBL_MAX, DBL_MIN subnormal, -0.0, 1/3, NaN, ±inf all round-trip
  bit-exact through both serialize (%.17g) and dump (raw f64 bits).

Assessment: this subsystem is unusually hardened. Only one minor inconsistency found (F1).

## FINDINGS
### F1 [LOW] json.parse: inconsistent lone-surrogate handling (throws vs U+FFFD substitution)
- where: src/kirito/stdlib_json.hpp:143-154
- repro:
    var json = import("json")
    json.parse("\"\\uD800\"")        # => len 1 (silently substitutes U+FFFD)   OK
    json.parse("\"\\uD800\\u0041\"") # => THROW "invalid low surrogate in \u escape"
    json.parse("\"\\uD800\\uD800\"") # => THROW
- actual: a lone high surrogate followed by NO `\u` is silently replaced with U+FFFD (per the code
  comment's stated WHATWG-style leniency), but a high surrogate followed by a `\u` escape that is NOT
  a valid low surrogate (a normal char like `A`, or another high surrogate) THROWS instead of
  applying the same U+FFFD substitution.
- expected: consistent policy. If the design is "always yield well-formed UTF-8 by substituting FFFD"
  (as the comment at :151-154 says), the high+non-low case should also substitute FFFD for the high
  surrogate and continue processing the following escape, matching browsers/Python(surrogatepass)/
  most JSON readers. As-is, `"\uD800X"` (X escaped) is rejected while `"\uD800"` at end is accepted.
- fix idea: when the paired `\u` low is out of the 0xDC00-0xDFFF range, don't fail — set cp=0xFFFD
  for the high surrogate, DON'T consume the second escape (rewind the `pos_ += 2`), and let the loop
  re-read it normally. Low severity: only affects malformed-surrogate JSON, never well-formed input.
