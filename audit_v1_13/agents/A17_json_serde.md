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

## Findings

### A17-1: Set/Dict containing a content-hashed value round-trips into the WRONG hash bucket (silent membership corruption)
- severity: **medium** (silent data corruption on a normal round-trip; not a crash)
- location: `src/kirito/stdlib_serde.hpp:213-234` (rebuild pass 2) vs `:236-246` (pass 3); interacts
  with `src/kirito/collections.hpp:251` (`SetVal::add` hashes eagerly) and `:143` (`DictVal::set`).
- category: correctness / deserialization integrity
- description: `rebuild` wires a Set's elements / a Dict's keys in **pass 2**, but a `Stateful` value's
  real payload is only restored by `_setstate_` in **pass 3**, and a `Tag::Object` instance's attributes
  are filled in pass 2 **at its own (higher) index**. Because `flatten` reserves a container's id
  *before* its children, the container always has a lower index than its freshly-encountered elements,
  so in the ascending pass-2 loop the element is inserted into the Set/Dict **before** its state/attrs
  exist. `SetVal::add`/`DictVal::set` compute and bucket by `hash()` at insertion time. For any element
  whose hash is **content-based**, the element is bucketed under its *empty* hash and then mutated, so a
  later `hash()` lands in a different bucket. Concretely triggered by:
  - a **Set of `Bytes`** or **Dict keyed by `Bytes`** (`Bytes::hash` = `std::hash<string>(data)`,
    factory builds an empty Bytes, `_setstate_` fills it — bytes.hpp:68);
  - a **Set/Dict of `DateTime`** (hash by epoch; empty epoch=0 at insert — stdlib_time.hpp:173);
  - similarly Complex/Matrix/Tensor Stateful values if hashable;
  - a **user class with an attribute-dependent `_hash_`** used as a Set member / Dict key
    (class_value.hpp:111 — hashable iff `_hash_` defined; attrs filled at the element's own pass-2 index,
    which is > the container's).
- failure-scenario: `var d = import("dump"); var s = {Bytes([1]), Bytes([2])};
  var back = d.loads(d.dumps(s)); back.contains(Bytes([1]))` → **False** (element is in the wrong
  bucket). Same for `serialize`. Dict: `{DateTime-key: v}` round-trips but `back[key]` KeyErrors.
- proposed-test: round-trip `{b"a", b"b"}` and assert `.contains(Bytes([...]))` True for each member,
  and `len` + iteration agree; round-trip a Dict keyed by `Bytes`/`DateTime` and assert key lookup
  succeeds; round-trip a Set of a user class with attr-based `_hash_`. (Current tests only cover
  Sets/Dicts of scalar ints/strings, whose value is fully materialised in pass 1 — masking the bug.)
- proposed-fix: after pass 3 (state/attrs all restored), **re-insert** every Set element / re-key every
  Dict whose members include Object/Stateful nodes (rehash), or defer wiring of Sets/Dicts until after
  pass 3. Simplest: run Set/Dict wiring in a final pass after `_setstate_` and Object-attr filling.
- confidence: **high** (mechanism verified end-to-end: reservation order, eager hashing, content hashes,
  3-pass ordering).

### A17-2: `rebuild` imposes no depth/expansion cap; a hostile blob can build an arbitrarily deep structure
- severity: low
- location: `src/kirito/stdlib_serde.hpp:159-248` (no depth bound; only `flatten` — the encode side —
  is depth-limited)
- category: robustness / defense-in-depth
- description: `flatten` caps nesting at 10000, but `rebuild` has no equivalent cap — a directly-crafted
  blob (chain of N List nodes each linking the next) reconstructs a depth-N structure bounded only by
  node count (≈ blob size). Because GC mark is iterative and equals/stringify are depth-guarded, this
  does not itself overflow the stack today, so impact is low. But nothing prevents a >10000-deep
  structure from being produced by deserialization even though the same structure cannot be *serialized*
  (asymmetry) — a future unguarded recursive consumer would then be reachable only via a malicious blob.
- failure-scenario: hand-crafted "KDMP" with 50000 chained List nodes → deserializes to a 50000-deep
  list; any later unguarded recursive traversal risks overflow.
- proposed-test: craft a deep chained blob, load it, and assert it either loads with a bounded structure
  or throws "too deeply nested" — pin the intended contract.
- proposed-fix: mirror the flatten depth ceiling in rebuild (e.g. bound the reconstructed depth, or
  simply document that consumers must be depth-guarded and add a rebuild-side cap for symmetry).
- confidence: medium (real asymmetry; low current impact given iterative GC + guarded consumers).

### A17-3: Deserializing untrusted `dump`/`serialize` data constructs arbitrary in-VM class instances and invokes their `_setstate_` (pickle-class trust hazard)
- severity: low/medium (by-design, but under-documented as a security boundary)
- location: `src/kirito/stdlib_serde.hpp:181-209` (findClass by name → bare InstanceValue, `_init_`
  bypassed) + `:236-246` (`_setstate_` called with attacker-controlled state)
- category: security / trust boundary
- description: a blob naming any class defined in the VM causes `rebuild` to build a bare instance and
  set **arbitrary attributes** directly (bypassing `_init_`/invariants, Tag::Object at :225-233), or to
  call `_setstate_(state)` with attacker-chosen state (Tag::Stateful). It cannot instantiate arbitrary
  C++ types (only registered deserializers) and cannot run `_init_`, but a class with a side-effecting
  `_setstate_`, or invariants enforced only in `_init_`, is exploitable by a malicious blob. This is the
  standard "never unpickle untrusted data" property and is not called out as a security caveat.
- failure-scenario: an app that `dump.load`s a user-supplied file, where some class's `_setstate_` (or a
  method later run on a corrupted-attr instance) performs I/O / assumes invariants → misbehaviour.
- proposed-test: n/a (design); add a doc note + a test asserting attribute-based reconstruction bypasses
  `_init_` (pins the documented behaviour).
- proposed-fix: document `dump`/`serialize` `loads`/`load` as unsafe on untrusted input in the stdlib
  reference (as Python does for `pickle`); optionally an opt-in allowlist of deserializable class names.
- confidence: high (behaviour is explicit in the code).

### A17-4: `serialize.loads` error-message translation omits `stoul`/`stod` variants used by the reader
- severity: info/cosmetic
- location: `src/kirito/stdlib_serialize.hpp:130` (`std::stoul` for rootId), `:105` (`parseDouble`),
  vs the translation list at `:182` (`"stoi"|"stol"|"stoll"|"stod"|"stoull"`)
- category: diagnostics quality
- description: a malformed rootId token throws `std::stoul`/`out_of_range` whose `what()=="stoul"` (and
  `parseDouble` may surface other messages) — not in the translation set, so the user sees
  `corrupt serialized data: stoul` instead of `…: malformed number`. Still a clean catchable error (no
  crash), just an unpolished message. (`countToken` also returns `int` bounded by `s_.size()` cast to
  `long`; only a >2GB text blob could narrow badly — negligible.)
- failure-scenario: `serialize.loads("KSER1 1 N x")` (bad rootId) → message "…: stoul".
- proposed-test: assert the message is user-readable for a bad-rootId blob.
- proposed-fix: add `"stoul"`,`"stod"` (and catch-all: any `sto*` → "malformed number") to the map.
- confidence: high.

## Coverage gaps (tests to add)

- **A17-1 gap (priority):** no round-trip test for Set/Dict whose elements/keys are content-hashed
  (`Bytes`, `DateTime`, user class w/ `_hash_`). Existing set/dict round-trips use scalar int/string
  members only, which mask the pass-2/pass-3 ordering bug.
- json.stringify of **Set / Bytes / user-instance / Matrix** on the **compact** path (only the
  indent>0 path's Set/Function rejection is tested in test_serialization_deep.cpp:51-52); compact-path
  throws are untested.
- json **unpaired-surrogate → U+FFFD** substitution (stdlib_json.hpp:151-154) and **invalid low
  surrogate throw** (:149) — no test.
- json `readHex4` malformed `\u` (non-hex digit / fewer than 4 digits at EOF) — no test.
- json **duplicate object keys** (last-wins), **deeply-nested `[[[…` → "nesting too deep"** (parse-side
  depth guard) — no direct test.
- serialize/dump: round-trip of native value types **Matrix / Complex / DateTime / Random / Tensor**
  and **Bytes** through both codecs (the Stateful path) — not exercised in the C++ unit tests
  (only user-class `_getstate_`/`_setstate_` and scalars/containers are).
- serialize/dump: **Dict-key/Set-member being a user instance** (attr-hash) — untested.
- rebuild **child-id-out-of-range in a Dict/Set/Object link** (only List and root-id are tested,
  test_serialize.cpp:87-88) — add a blob with an OOB dict-key id / object-attr id.
- binary `dump`: a hand-crafted blob with a **huge List count `c`** (e.g. 0xFFFFFFFF) truncated —
  assert clean "truncated dump data" (implicitly fuzzed, not pinned deterministically).
- A17-2: crafted deep chained blob (rebuild depth) — no test pins the intended contract.

## DRY note

json and serde/dump are correctly separated: json has its own value/writer model (it is flat
interchange, no ref/cycle preservation), while `serialize` and `dump` share the single
`serde::flatten`/`rebuild` core and supply only their byte codec — verified single-source, no
divergence between the text and binary tag handling (both cover all 10 tags; text 'N/B/I/F/S/L/D/T/O/P'
maps 1:1 to binary 0..9).
