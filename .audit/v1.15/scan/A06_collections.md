# A06 Collections

Status: IN PROGRESS

Scope: List/Dict/Set (+ internal Array) — literals, indexing, slicing, iteration, `in`, all methods,
lexicographic list ordering, `+`/`*`, hashing, ProbeScope reentrancy guard, stable sort.
Files: `src/kirito/collections.hpp`, `src/kirito/runtime.hpp` (List/Dict/Set getAttr + sort/apply/
enumerate/zip/map/filter/sorted/reversed), helpers in `src/kirito/native.hpp` (`sliceIndices`,
`sequenceIndex`, `singleKey`), `src/fum/unordered_map.hpp` / `unordered_set.hpp` (vendored map).

Read first: `.audit/README.md` false-positives table (NaN write-only keys, unordered Dict/Set
iteration, tabular NaN-key handling, etc.) — none of those re-flagged below.

Baseline read: `tools/tests/unit/test_collections.cpp`, `test_apply.cpp`, `test_list_ops.cpp`,
`test_references.cpp`, plus `tools/tests/scripts/spec_reentrant_clear.ki`, `spec_iter_mutation.ki`,
`spec_audit_hardening.ki`, `class_hash.ki`. This subsystem has been through several prior audit
rounds (v1.12.1 A08/A09 fixes: Dict/Set stringify UAF, reentrant clear()/pop() during a probe,
empty-bucket reclamation, rooted List.index/count/remove/contains snapshots, Set union-family
rooting) — the mechanics below were re-derived from scratch and cross-checked against that history,
not assumed.

## Design notes verified (not bugs, recorded so I don't re-flag them)

- `ProbeScope`/`probing_` correctly protects only *mutation* during a live bucket probe; reads
  (`find`/`contains`) nest freely by design (comment in collections.hpp:122-128), confirmed correct:
  nested reads just re-enter `ProbeScope` (prev value true, restored true), nested *mutation* is
  rejected via the `if (probing_) throw` guard in `set`/`remove`/`clear`/`add`/`discard`/`pop`.
- `DictVal::set`/`SetVal::add` call `k.hash()` (which may run a user `_hash_`, arbitrary Kirito code)
  **before** taking any reference into `buckets`/the target bucket vector — so a reentrant mutation
  triggered from inside `_hash_` itself is safe (no reference is live yet to dangle). Confirmed by
  re-reading the exact statement order in `DictVal::set`/`find`/`SetVal::add`/`contains`.
- The vendored `fum::unordered_map`/`unordered_set` store elements in a stable segmented node arena,
  so a `std::vector<...>&` reference to one bucket survives a rehash of the *outer* table triggered by
  inserting into a *different* bucket. The actual hazard `ProbeScope` guards is a nested mutation that
  `push_back`/`erase`s **the same bucket vector** currently being probed — confirmed that's exactly
  what's blocked.
- `DictVal::equals`: the `Handle* ov = o.find(...)` pointer is dereferenced (`arena.deref(*ov)`) as a
  function-argument expression that fully evaluates (copying the `Handle` by value) before
  `.equals()` executes and can run a reentrant `_eq_` that mutates `o` — so no UAF window there
  despite `o` not being probing-guarded across the whole comparison loop (only within `o.find`
  itself). Traced the exact evaluation-order guarantee (C++17 argument-before-call sequencing).
- Every `List`/`Dict`/`Set` mutator that can run arbitrary user code (`apply`, `sort`, `remove`,
  `index`, `count`, `contains`, `extend` via `_eq_`/key-fn) snapshots `elems`/`pairs`/`items` into a
  local vector, roots it via `RootScope`, and only writes results back into the (re-fetched) live
  container afterward — verified this pattern is applied uniformly, including the newer `sort(key=…)`
  path (Schwartzian-transform keys are rooted too).
- Fresh containers allocated inside a method body (`ListVal::binary` Add/Mul, `apply`'s `out`,
  `union`/`intersection`/etc.'s `result`, the temporary `SetVal otherSet` stack locals in the set-
  algebra methods) are always young (`gcAge_` defaults 0), so `gcWriteBarrier` early-outs on them with
  a single byte test and never touches the arena — including for the **stack-local** (non-arena)
  `SetVal otherSet` used by `union`/`intersection`/`difference`/`symmetricdifference`/`issubset`/
  `issuperset`/`isdisjoint`: confirmed this is safe by construction, not a latent bug, because the
  write-barrier's young check short-circuits before any arena/container-identity access.
- `xs.extend(xs)` and `xs.apply(...)`/`xs.sort(key=fn)` where `fn` clears/mutates `xs`: safe by the
  snapshot pattern above — `ListVal::iterate()` returns `elems` **by value** (the `optional<vector>`
  copies it), so `extend`'s `other` is already a frozen copy before the append loop starts appending
  into the live list.
- `[[0]] * n` shares one inner-list handle across all outer slots — documented, matches Python, tested
  implicitly by the aliasing tests in `test_references.cpp`.
- Huge repetition (`[0]*10**9`, `Bytes(...)*n`, string padding) is bounded by the shared `kMaxRepeat`
  cap (`common.hpp`), applied consistently in `ListVal::binary` Mul, `StrVal`/`BytesVal` `*`/pad/
  join/replace/format paths.
- `xs[::0]` throws "slice step cannot be zero"; `xs[::-1]` and other slices go through the shared,
  overflow-hardened `sliceIndices` (native.hpp) — a near-`INT64_MAX`/`INT64_MIN` step is
  count-driven (not `i += step` in a loop), so no signed-overflow UB. Already covered by
  `test_adversarial.cpp` (zero-step) and multiple `.ki` scripts (`[::-1]`, `[::2]`) across types.

## Findings

### A06-1: `Dict.remove()` on an unhashable key reports the wrong error  [severity: LOW] [confidence: confirmed]
- **Location**: `src/kirito/collections.hpp:230-246` (`DictVal::remove`) vs. `src/kirito/runtime.hpp:787-795` (the `"remove"` getAttr binding)
- **What**: `DictVal::remove()` silently returns `false` for an unhashable key
  (`if (!k.hashable()) return false;`, collections.hpp:232) — unlike `DictVal::set`/`find`, which call
  `requireHashable()` and throw `"unhashable type 'X'"`. The `remove` method wrapper then turns that
  `false` into `throw KiritoError("key not found: " + vm.stringify(a[0]))` (runtime.hpp:792-793). So
  `d.remove([1,2])` (a List key, unhashable) throws **"key not found: [1, 2]"** instead of
  `"unhashable type 'List'"` — misleading, since the real reason isn't absence, it's that the key can
  never be looked up at all. Every sibling entry point (`get`, `pop`, `setdefault`, `d[k]`, `d[k]=v`,
  all of which route through `find`/`set`) correctly say "unhashable type". `Set.remove` (runtime.hpp:897)
  was explicitly hardened in an earlier round to say "unhashable type" too (the comment there even
  calls out that it "had drifted to a bare 'unhashable type' with no type name") — `Dict.remove` is the
  one entry point that was missed, and it doesn't even say "unhashable", it says "key not found".
- **Repro**:
  ```
  var d = {}
  d.remove([1, 2])   # throws "key not found: [1, 2]" — should say unhashable type 'List'
  ```
- **Proposed fix**: In `DictVal::remove`, call `requireHashable(k)` (throwing, like `set`/`find`)
  instead of `if (!k.hashable()) return false;`; or, minimally, have the `"remove"` getAttr binding
  check hashability itself before calling `remove()` and throw the canonical message (mirroring what
  `Set.remove`'s binding already does at runtime.hpp:897).
- **Proposed test**: `assert throws-with-message(Function(): {}.remove([1,2]), "unhashable")` — a
  `.ki` regression alongside `spec_reentrant_clear.ki`/`spec_audit_hardening.ki`'s error-message
  pinning style, or a C++ `CHECK` in `test_collections.cpp` asserting the exception text contains
  "unhashable", not "key not found".

### A06-2: DRY — the Set-algebra methods rebuild an identical temporary `SetVal` five times  [severity: LOW] [confidence: confirmed]
- **Location**: `src/kirito/runtime.hpp:949-1009` (`union`/`intersection`/`difference`/
  `symmetricdifference`/`issubset`/`issuperset`/`isdisjoint`)
- **What**: Six of the seven Set-algebra methods each contain their own copy of
  `SetVal otherSet; for (Handle e : other.value()) otherSet.add(vm.arena(), e);` — used to get O(1)
  membership tests against `other` when `other` is an arbitrary iterable (not necessarily a Set). This
  is the same "materialize an iterable into a lookup set" idiom repeated five times verbatim (union,
  intersection, difference, symmetricdifference — twice within the same lambda — issubset/issuperset/
  isdisjoint share one more copy). Not a bug (each copy is independently correct and independently
  rooted), but a straightforward DRY violation matching the kind of pattern the project already
  extracts elsewhere (`probeBucket`, `sliceIndices`, `singleKey`).
- **Repro**: N/A (style, not behavior).
- **Proposed fix**: Factor a small helper, e.g.
  `inline SetVal materializeSet(KiritoVM& vm, const std::vector<Handle>& elems)` (or taking the
  iterate() result directly), and call it once per method.
- **Proposed test**: N/A (refactor; existing set-algebra tests already pin behavior).

## Coverage gaps (method × scenario matrix not fully exercised)

Legend: [x] tested, [~] indirectly/partially covered, [ ] gap.

| Method | empty | single | negative-idx | aliased/self | reentrant-mutation | bad-type-arg |
|---|---|---|---|---|---|---|
| `List.append/pop/insert/remove/index/count/extend` | [x] | [x] | [x] (pop/insert) | [x] `xs.append(xs)` (str/eq) | [x] (index/count/remove rooted, A09-1) | [~] (insert index type checked; append/extend arg-type mostly untested) |
| `List.sort(key=…)` | [~] | [~] | n/a | n/a | [x] key that **appends** (spec_audit_hardening.ki) — **[ ] key that `xs.clear()`s mid-sort is untested** (safe by inspection, same code path, but not pinned) | [~] non-callable `key` untested (only non-callable in `apply`/`filter`) |
| `List.apply(fn)` | [x] | [x] | n/a | [ ] `xs.apply(fn)` where `fn` clears `xs` — untested (safe by inspection: snapshot taken before fn runs) | [ ] same as above | [x] (non-callable) |
| `List * n` / `+` | [x] (`[]+[1]`, `[]*n` implicit) | [x] | n/a | [ ] `[[0]]*n` shared-reference explicitly asserted (only described in comments, not asserted in a test) | n/a | [x] |
| `List <`/`<=`/`>`/`>=` | [~] | [x] | n/a | [ ] self-comparison `xs < xs` where `xs` is self-referential (cyclic) — untested; `kiLessThan`'s `EqualsGuard` bounds it but the exact behavior (throw vs. hang) on a **cyclic** list compared for order is unverified | [ ] | [x] (non-List rhs) |
| `Dict.get/pop/setdefault` | [x] | [x] | n/a | n/a | [x] (spec_reentrant_clear.ki covers `d[k]=v`; get/pop/setdefault not separately reentrancy-tested but share `find`) | [x] unhashable → throws (via `find`) |
| `Dict.remove` | [ ] empty dict | [x] | n/a | n/a | [ ] | **[ ] unhashable-key error message wrong — see A06-1** |
| `Dict.update(other)` | [x] | [x] | n/a | [~] `d.update(d)` — safe by inspection (`pairs()` snapshots), **not asserted in a test** | [ ] | [x] non-Dict/non-iterable throws |
| `Dict.popitem` | [x] throws on empty | [x] | n/a | n/a | [ ] | n/a |
| `Set.add/discard/remove/contains/pop` | [x] | [x] | n/a | n/a | [x] (spec_reentrant_clear.ki: add/pop) | [x] `remove` throws "unhashable type"; **`discard` on an unhashable value silently no-ops (runtime.hpp:926) — not explicitly tested, and worth confirming this asymmetry (discard tolerant, remove strict) is the intended contract, not drift** |
| `Set.union/intersection/difference/symmetricdifference` | [x] | [x] | n/a | [x] `s.union(s)`/`s.difference(s)` semantics correct by inspection (`iterate()` snapshots) — **not asserted with self-argument in a test** | [ ] a colliding `_hash_`/`_eq_` element during `otherSet.add(...)` inside these methods — untested (should behave like plain `Set.add` reentrancy, by inspection) | [x] non-iterable throws |
| `Set.issubset/issuperset/isdisjoint` | [x] | [x] | n/a | [ ] self-argument | [ ] | [x] |
| `Set -`/`<`/`<=`/`>`/`>=` operators | [x] | [x] | n/a | [ ] `s - s` / `s < s` | n/a | [x] non-Set rhs falls to base "unsupported operator" |
| Hashing / `d[k]=v` where `k._hash_` mutates `d` | n/a | n/a | n/a | n/a | [x] (spec_reentrant_clear.ki, spec_audit_hardening.ki) | n/a |
| Cyclic containers in `==`/`str`/`sort` | n/a | n/a | n/a | [x] `==` depth-bounded throw (test_references.cpp), `str` ellipsis (test_collections.cpp) | n/a | [ ] a cyclic list passed to `sorted()`/`.sort()` (not just `==`) — untested; `kiLessThan` recurses through `EqualsGuard` the same way, so likely throws cleanly, but unverified |

## Summary

Findings: 1 LOW confirmed bug (A06-1: `Dict.remove()` on an unhashable key produces a misleading
"key not found" message instead of "unhashable type", inconsistent with every sibling entry point and
with `Set.remove`'s already-fixed message), plus 1 LOW DRY nit (A06-2: the Set-algebra methods rebuild
an identical temporary lookup-Set five times; a one-line extraction would remove the duplication).

No memory-safety, aliasing, GC-barrier, or ProbeScope-reentrancy bugs were found — the subsystem has
already been through several hardening rounds (v1.12.1 A08/A09) and the mechanics (probe-guard
ordering, snapshot-before-mutate, write-barrier young-object early-out, vendored map's stable node
arena) all check out under adversarial tracing. The larger opportunity here is coverage: several
"safe by inspection" paths (sort/apply with a self-clearing callback, `d.update(d)`, Set-algebra
methods called with the receiver itself as the argument, a cyclic list passed to `sorted()`) are not
pinned by an explicit regression test even though the underlying snapshot/rooting pattern that makes
them safe is exercised elsewhere for adjacent methods — see the coverage table above for the specific
gaps worth adding tests for before the next round re-derives this from scratch.
