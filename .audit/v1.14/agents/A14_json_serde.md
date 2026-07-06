# A14 — json + serde audit

Scope: `src/kirito/stdlib_json.hpp`, `src/kirito/stdlib_serde.hpp` (shared `flatten`/`rebuild`),
`src/kirito/stdlib_serialize.hpp` (text `serialize`/KSER1), `src/kirito/stdlib_dump.hpp` (binary `dump`/KDMP).
Method: READ-ONLY on src; every finding confirmed with `build-debug/ki` probes.

## Surface enumerated
- json: `parse`/`loads`, `stringify`/`dumps(value, indent=0)`. Lenient number grammar (leading zero,
  trailing dot, empty exponent — regression-pinned), \u + surrogate pairs, U+FFFD for lone surrogates,
  NaN/Infinity/-Infinity accepted+emitted, control chars \u-escaped, shortest round-tripping floats,
  depth guard 1000, cycle guard (`active` path set), non-serializable→throw.
- serde::flatten/rebuild: Node table, identity ids, cycles, 4-pass rebuild (content-hashed Set/Dict
  members deferred to pass 4 per A17-1). Instance→_getstate_ (Stateful) or attrs (Object). Native
  opt-ins verified: Bytes, Matrix/Complex/ComplexMatrix, Tensor (grad→throw), DateTime, Random.
- serialize (text KSER1) + dump (binary KDMP v1) share the core; differ only in codec.

## What is SOLID (confirmed, not bugs)
- json malformed rejection: trailing comma, unquoted/single-quoted key, bare `{`, `.5`, `tru`, empty
  input, BOM, `[1 2]`, trailing chars — all throw clean `JSON parse error: …`.
- json big int → Float widening; `1e400` → inf; lone surrogate → U+FFFD; dup keys → last wins.
- json NaN/inf round-trip; non-string dict keys coerced to canonical JSON tokens; Set/function → throw.
- json deep-nest parse AND stringify both guard (throw, no overflow).
- serde: self-ref list, diamond (shared child once), mutual dict refs, cycle THROUGH a Set/Dict all
  round-trip on both text+binary. Class round-trip: empty class, private members, instance/Bytes Dict
  keys, instance Sets. Native value types round-trip; grad-Tensor + BytesIO/Regex/File → clean catchable
  throw. Class-not-in-registry-on-load → clean error. Corruption/truncation/fuzz (1200 garbage inputs
  across binary+text) → 100% clean throws, zero crashes. Out-of-range root/child ids, negative/huge
  counts all rejected.

---

### A14-1: `_setstate_` reads an EMPTY state container when the state Dict/Set is keyed by a content-hashed member (pass-3-before-pass-4 ordering hole)
- severity: **Medium**
- location: `stdlib_serde.hpp:256-281` — Pass 3 (`_setstate_`) runs BEFORE Pass 4 (wiring content-hashed
  Set/Dict members). The comment at lines 216-218 explicitly promises "a `_setstate_` that reads its
  state Dict still sees it populated" — that guarantee is FALSE for a content-hashed-keyed container.
- category: bug / correctness (silent data corruption on deserialize)
- description: A `_getstate_` may legitimately return a Dict keyed by a `Bytes`/instance/Matrix (or a
  Set of such), i.e. a "content-hashed member" that A17-1 defers to pass 4. But `_setstate_` is invoked
  in pass 3, which runs *before* pass 4. So during restore the state container is still EMPTY. If
  `_setstate_` merely stashes the reference (`self.m = state`) the value is fine (pass 4 fills it later),
  but if `_setstate_` READS THROUGH the container (`for k in state: total += state[k]`) it sees nothing.
  String/scalar-keyed state (wired in pass 2) is unaffected — hence most native `_setstate_`
  implementations (which read a List-of-scalars state) are safe; the hole is specific to a
  content-hashed-keyed state container read during restore.
- failure-scenario (CONFIRMED): a class whose `_getstate_` returns `{Bytes([9]): 42}` and whose
  `_setstate_` computes `sum(state.values())` — after `serialize.loads(serialize.dumps(obj))` the sum is
  **0**, not 42. String-keyed state and store-the-reference both give the correct 42. Reproduced on
  both `serialize` and `dump`.
- proposed-test: add to `spec_serde_adversarial.ki`: a class with a Bytes-keyed (and an instance-keyed)
  state Dict whose `_setstate_` reads through it during restore; assert the read value survives.
- proposed-fix: either (a) run pass 4 (wire deferred content-hashed containers) BEFORE pass 3
  (`_setstate_`) — but pass 4 needs the members fully materialised, which for a Stateful member requires
  pass 3, so a naive reorder risks the A17-1 bug returning; or (b) walk the reachable content-hashed
  containers of each Stateful node's state and force-wire them immediately before its `_setstate_` call;
  or (c) at minimum, correct the misleading comment and DOCUMENT that a `_setstate_` must not read
  through a content-hashed-keyed state container during restore (store-the-reference is the contract).
- confidence: **High** (mechanism traced in code + empirically reproduced; the in-code comment asserts
  the opposite behavior, so this is a genuine contract violation, not intended).

### A14-2: `json.stringify` indent path — unguarded signed-int overflow in `(depth+1)*indent` + ugly diagnostic on a large indent
- severity: **Low-Medium**
- location: `stdlib_json.hpp:320` (`std::string pad((depth + 1) * indent, ' ')`, same at `padEnd`);
  `indent` comes from `stdlib_json.hpp:370` (`static_cast<int>(args[1].asInt(...))`, unbounded).
- category: bug / robustness (UB + poor diagnostic; adversarial input to a serializer)
- description: `indent` is an unbounded `int`. `depth` is bounded to ~1000 by the `active.size() > 1000`
  guard (line 318), so `(depth+1)*indent` can reach ~1001*indent. For `indent > ~2.1e6` at depth ~1000
  this **overflows `int` (signed-overflow UB)** — a UBSan trap under the `asan` preset (latent: no test
  exercises it). Separately, a large indent at any depth (e.g. `indent=2e9`) produces a
  `std::string(huge, ' ')` whose `std::length_error`/`bad_alloc` surfaces to Kirito as the raw,
  unhelpful message `basic_string::_M_create` rather than a clean diagnostic.
- failure-scenario (CONFIRMED): `json.stringify([[1]], 2000000000)` → catchable but ugly
  `basic_string::_M_create`. `json.stringify(deepNestedList, 3000000)` overflows the multiply (UB; would
  abort under UBSan). Negative indent is already handled (→ compact) and tested; only the large-positive
  edge is unguarded.
- proposed-test: `assert throws(Function(): return json.stringify([[1]], 3000000))` with a clean message;
  add a huge-indent case to `audit_json.ki`.
- proposed-fix: clamp/validate indent once at the entry, e.g.
  `if (indent > 1024) throw KiritoError("json.stringify: indent too large (max 1024)");` (or any sane
  cap) — eliminates both the UB and the ugly message. Negative already maps to compact.
- confidence: **High** on the overflow/diagnostic mechanism; Medium on real-world impact (needs
  pathological input, but it is a UBSan-latent trap on a security-adjacent serializer surface).

### A14-3: json shared-DAG (non-cyclic) has no expansion guard — a tiny structure explodes exponentially
- severity: **Low** (informational — matches Python `json.dumps`, but worth a bound)
- location: `stdlib_json.hpp:268-303`/`306-351` — `active` tracks only the CURRENT path (cycle guard);
  a value reached by two paths is re-serialized once per path (json is flat, no ref preservation).
- category: robustness / resource exhaustion (DoS)
- description: The only stringify guards are cycle-detection (`active.count`) and depth (`active.size() >
  1000`). A shared **acyclic** DAG defeats both: build `node = [node, node]` 24 times (≈48 list objects,
  a trivially small in-memory structure) and `json.stringify` must emit 2^24 ≈ 16M elements.
- failure-scenario (CONFIRMED): a 24-level doubling DAG hangs `json.stringify` past a 15s timeout
  (terminated). depth stays 24 so the depth guard never fires.
- proposed-test: bound the total emitted size / node-visit count and assert a clean throw on a doubling
  DAG (or explicitly document the flat-expansion behavior as accepted).
- proposed-fix: OPTIONAL — cap total output length or total node visits during `write`/`writeIndented`
  and throw "structure too large to serialize to JSON". (Python has the same footgun; this may be an
  accepted tradeoff, hence Low.)
- confidence: High (reproduced); Low on whether a fix is wanted.

### A14-4 (coverage gap): no test exercises the A17-1 content-hashed-member path with a _setstate_ read-through, nor a large json indent
- severity: Low (coverage)
- location: tests — `tools/tests/scripts/spec_serde_adversarial.ki`, `r5_serde.ki`, `audit_json.ki`;
  `tools/tests/unit/test_serde.cpp`.
- category: coverage gap
- description: Grep across the json/serde `.ki` + `.cpp` suites shows: (1) the content-hashed-keyed
  container inside a `_getstate_` state, read through by `_setstate_` (the A14-1 bug), is untested —
  the existing A17-1 tests cover cycles-through-Set/Dict but not a Stateful whose STATE is such a
  container; (2) json `indent` is tested for negative (compact) and small positive (2/4) but never for a
  large/overflowing value (the A14-2 edge). Both new behaviors above would be caught by dedicated tests.
- proposed-test: as listed in A14-1 / A14-2.
- confidence: High (grep-confirmed absent).

## Summary
- Bugs: **2** — A14-1 (Medium: `_setstate_` reads empty content-hashed state container; contract
  comment is wrong), A14-2 (Low-Medium: json indent signed-overflow UB + ugly diagnostic).
- Robustness/informational: **1** — A14-3 (Low: json shared-DAG exponential blowup, no guard).
- Coverage gaps: **1** — A14-4.
- Verified SOLID (no defect): json malformed/surrogate/bignum/depth handling, serde cycles/diamonds/
  mutual-refs/Set-cycles, class + native round-trips, grad-Tensor/resource-like throws,
  class-not-registered error, and 1200-input corruption/truncation/fuzz (zero crashes).
- The A17-1 deferral logic is correct for CYCLES and shared refs; the one hole it leaves is A14-1
  (a `_setstate_` that reads through — not just stores — a content-hashed-keyed state container).
