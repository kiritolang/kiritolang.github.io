# A09 — Collections Audit (List / Set / Dict / Array + hashing)

Auditor: A09 (read-only static audit). Scope: `src/kirito/collections.hpp`,
`src/kirito/hashing.hpp`, and the List/Set/Dict/Array method + operator + iteration
implementations in `src/kirito/runtime.hpp`. Existing tests:
`tools/tests/unit/test_list_ops.cpp`, `test_sort.cpp`, `test_collections_deep.cpp`,
`tools/tests/scripts/` golden scripts.

Status: COMPLETE. 6 findings (1 High, 1 Medium, 1 Low-Medium, 3 Low) + coverage matrix.

---

## Findings

### A09-1: List value-search methods (remove / index / count / `in`) can UAF via a reentrant `_eq_`

- **severity**: High (memory safety — use-after-free)
- **location**: `src/kirito/runtime.hpp` `ListVal::getAttr` — `remove` (589-597), `index` (598-617),
  `count` (642-651); and `ListVal::contains` (1609-1613).
- **category**: bug / iterator invalidation / GC-and-realloc safety
- **description**: These methods cache a live reference to the backing vector
  (`auto& e = self_list(vm, self).elems;`, or iterate the member `elems` directly in `contains`)
  and then call `kiEquals(vm, e[i], a[0])` inside the loop. `kiEquals` dispatches to a user class's
  `_eq_` (via `viaEq`), which is arbitrary Kirito code that may mutate the *same* List
  (`append`/`clear`/`insert`). Any such mutation can reallocate `elems`, leaving the cached `e`
  reference (and, in `contains`, the range-for `begin()/end()` iterators) dangling — subsequent
  `e[i]` / `e.size()` read freed memory. The maintainers already guard the analogous Dict/Set probe
  paths with `ProbeScope` (collections.hpp 119-126, tested in `test_audit_hardening.cpp`) and snapshot
  the List `sort`/`apply` paths (runtime.hpp 545, 511), but the List *value-search* methods got
  neither guard nor snapshot — an inconsistency that leaves this hole.
- **failure-scenario**: The hostile `_eq_` need not even be *in* the list — it need only be the
  searched-for value, since `kiEquals(e, value)` tries `value`'s `_eq_` too:
  ```
  class Evil:
      var _init_ = Function(self): pass
      var _eq_ = Function(self, other):
          xs.append(0)     # realloc xs mid-search
          return False
  var xs = [1, 2, 3, 4, 5, 6, 7, 8]
  xs.remove(Evil())        # or: Evil() in xs / xs.index(Evil()) / xs.count(Evil())
  ```
  Under ASan this reports heap-use-after-free; in release it is silent corruption.
- **proposed-test**: A CTest (asan) that runs the snippet above for each of remove/index/count/`in`
  and asserts either a clean catchable error or a correct result — no crash. Mirror the Dict/Set
  reentrancy block in `test_audit_hardening.cpp`.
- **proposed-fix**: Either (a) snapshot `std::vector<Handle> src = ...elems;` (rooted via a
  `RootScope`) and search the snapshot, as `sort`/`apply` already do, then translate the found
  position back; or (b) add a `ProbeScope`-style reentrancy guard on ListVal that rejects nested
  mutation during a value search; or (c) re-fetch `self_list(vm,self).elems` on every iteration
  (never cache the vector reference) and bound the index each step. (a) matches the existing List
  idiom most closely.
- **confidence**: High that the UAF is reachable; the exact release-mode symptom is
  allocator-dependent.

### A09-2: Sorting / min / max on a List containing NaN is undefined (non-strict-weak-ordering comparator)

- **severity**: Medium
- **location**: comparator `kiLessThan` (runtime.hpp 392-418) via `numericCompare` (136-150);
  used by `ListVal::sort` (`std::stable_sort`, 561), the `sorted` builtin (`std::stable_sort`, 3142),
  and `min`/`max`/`extremum`.
- **category**: bug / undefined behaviour / spec compliance
- **description**: `numericCompare` returns `2` (UNORDERED) when either operand is NaN, so
  `kiLessThan(a,b)` and `kiLessThan(b,a)` are *both* false for any pair involving NaN — i.e. NaN is
  treated as "equivalent" to every other number. That is not a strict weak ordering (NaN ~ 1 and
  NaN ~ 2 but 1 < 2), and `std::stable_sort`/`std::sort` have undefined behaviour when the comparator
  is not an SWO. In practice `stable_sort` (merge sort) does not read out of bounds, so the observed
  effect is a silently mis-ordered result rather than a crash, but it is standard-UB and the outcome
  is unspecified. `.compare()`/`==` semantics correctly keep NaN unordered, but sorting inherits the
  ill-defined ordering with no diagnostic.
- **failure-scenario**: `var a = [3.0, math.nan, 1.0, 2.0]\na.sort()` — result order is unspecified;
  `min([1.0, math.nan, 2.0])` likewise.
- **proposed-test**: A test that sorts a NaN-containing list and asserts a *defined* outcome
  (whatever the maintainers decide: either a clean throw, or NaNs sink/float to a fixed end). Today
  there is no such test (grep of `test_sort.cpp`/`spec_collections.ki`/`r4_collections.ki` finds none).
- **proposed-fix**: Give the sort/extremum comparator a total order over NaN (e.g. treat NaN as
  greater-than-all, matching numpy's sort), or reject NaN in ordering contexts with a clear error.
  Do NOT change `==`/`<` operator semantics.
- **confidence**: High that it is standard-UB and untested; Medium on user impact (mis-order, not crash).

### A09-3: `x in dict` throws on an unhashable `x` while `x in set` silently returns False — inconsistent membership

- **severity**: Low-Medium
- **location**: `DictVal::find` calls `requireHashable(k)` which throws (collections.hpp 152-154, via
  `DictVal::contains` 1614-1616); `SetVal::contains(const ObjectArena&, Handle)` returns `false` for
  an unhashable value (`if (!v.hashable()) return false;`, collections.hpp 263-266). `ListVal::contains`
  compares structurally and works for unhashable operands.
- **category**: bug / inconsistency (semantic)
- **description**: The three containers give three different answers for
  `<unhashable> in container`: List does a structural compare (works), Set returns `False` (lenient),
  Dict throws `unhashable type '...'` (strict). Set and Dict disagree, which is surprising for two
  hash-bucketed containers with otherwise parallel APIs. (CPython throws for both `[] in set()` and
  `[] in {}`.)
- **failure-scenario**: `[] in {1: 2}` throws; `[] in {1, 2}` returns `False`. A program that relies
  on membership never throwing (as Set allows) breaks when the container is a Dict, and vice-versa.
- **proposed-test**: Pin the chosen behaviour for both Set and Dict membership with an unhashable
  operand in `spec_collections.ki` / `r4_collections.ki`.
- **proposed-fix**: Pick one policy and apply it to both. Simplest is to make `DictVal::contains`
  mirror Set: return `false` for a non-hashable key instead of routing through the throwing `find`
  (add a `hashable()` short-circuit in `DictVal::contains`, not in `find`, so subscript `d[[]]` still
  throws as documented).
- **confidence**: High that the asymmetry exists; Medium on which side is "correct" (maintainer call).

### A09-4: Empty hash buckets are never reclaimed from Dict/Set on delete — unbounded map growth under churn

- **severity**: Low
- **location**: `DictVal::remove` (collections.hpp 213-227), `SetVal` `remove`/`discard`/`pop`
  (runtime.hpp 846-903). Each does `bucket.erase(...)` / `pop_back()` but never erases the now-empty
  bucket vector from the `buckets` map.
- **category**: weak spot / resource (slow leak + perf)
- **description**: With distinct-hash keys, every insert creates a `buckets[h]` entry; deleting the
  key empties the vector but leaves the map entry. `buckets.size()` therefore only ever shrinks on
  `clear()`. A long-running program that churns keys (insert/delete distinct keys repeatedly) grows
  the bucket map without bound, and `SetVal::pop` (which linear-scans buckets for the first non-empty
  one) degrades as empty buckets accumulate. Not a correctness bug (iteration/`keys()` skip empty
  buckets harmlessly), but a memory/perf cliff.
- **failure-scenario**: `var s = Set()` in a loop that `add`s then `remove`s a fresh Integer each
  iteration a few million times — resident memory climbs and `pop` slows, though `len(s)` stays small.
- **proposed-test**: A soak test asserting `buckets`/memory stays bounded under add+remove churn
  (would need a native probe; or just document as accepted).
- **proposed-fix**: In `remove`/`discard`/`pop`, `buckets.erase(it)` when the bucket becomes empty.
- **confidence**: High on the mechanism; Low on real-world impact.

### A09-5: `issubset` / `issuperset` / `isdisjoint` do not root the iterated `other` elements (inconsistent with the `union` family)

- **severity**: Low
- **location**: `SetVal::getAttr` (runtime.hpp 938-958). The `union`/`intersection`/`difference`/
  `symmetricdifference` branch first roots every element of `other.value()` via `rs.add(e)`
  (917, with an explicit comment about freshly-allocated String/Bytes/range handles), but the
  `issubset`/`issuperset`/`isdisjoint` branch builds `SetVal otherSet;` directly from
  `other.value()` with no `RootScope`.
- **category**: weak spot / GC-safety inconsistency
- **description**: For the builtin element types this is safe (hashing/`contains` of an
  Integer/String allocates nothing, so no GC runs mid-build). It only becomes unsafe if an element's
  `_hash_`/`_eq_` allocates (user instances) *and* the elements were freshly produced by the iterable
  (unrooted after `iterate()`'s internal `RootScope` unwinds). That combination is hard to reach
  through normal iterables (a user `_iter_` returns a List whose elements stay reachable), so this is
  latent rather than a live bug — but the asymmetry with the union family is a code smell worth
  closing.
- **failure-scenario**: Theoretical: an iterable that yields freshly-allocated instances whose
  `_hash_` triggers a collecting allocation between `otherSet.add` calls.
- **proposed-fix**: Root `other.value()` in the subset/superset/disjoint branch too (one shared
  helper for "materialize `other` into a rooted `SetVal`" would DRY 918-946 as well).
- **confidence**: Medium that it is currently safe; the inconsistency itself is certain.

### A09-6: DRY — Set.remove and Set.discard duplicate the hash→probe→erase body; Array is a phantom ValueKind

- **severity**: Low (maintainability)
- **location**: `SetVal::getAttr` `remove` (runtime.hpp 846-861) and `discard` (869-884) differ only
  in the not-found action (throw vs no-op) yet re-implement the whole
  `hashable? → hash → probing_ check → ProbeScope → find bucket → probeBucket → erase` sequence
  inline (which itself re-derives logic already in `SetVal`/`probeBucket`). The `copy()` bodies for
  List/Set/Dict (628-635, 748-754, 862-868) are also near-identical shallow-copy boilerplate.
- **category**: DRY
- **description**: A single private `SetVal::eraseIfPresent(arena, value) -> bool` would let `remove`,
  `discard`, and the `-`/set-op helpers share one guarded erase. Separately, `ValueKind::Array`
  (object.hpp 23) is referenced defensively in ~8 places (`kind()==Array` in runtime.hpp 263/402/430/458,
  value.hpp 165, stdlib_json.hpp, stdlib_tensor.hpp) but **no `Object` ever returns
  `ValueKind::Array`** — `ListVal::kind()` is hard-coded to `List` and there is no `ArrayVal` class.
  The "internal Array" described in CLAUDE.md does not exist as a distinct type; the `|| ...==Array`
  checks are dead branches. Either wire up the intended Array type or drop the phantom kind and its
  dead checks.
- **proposed-fix**: Extract the shared Set erase helper; remove or implement `ValueKind::Array`.
- **confidence**: High.

---

## Coverage assessment (method-by-method)

Existing tests are strong for the *happy path* and for previously-audited edge cases. Sources:
`test_sort.cpp`, `test_list_ops.cpp`, `test_collections_deep.cpp`, `test_audit_hardening.cpp`,
`spec_collections.ki`, `r4_collections.ki` (very thorough), `class_hash.ki`, `r6_hash.ki` (crypto
module, not value hashing).

**Well covered:**
- List: index/negative/OOB, slice (incl. negative step + bounds, float-bound throw, zero-step),
  concat/`+`, repeat/`*` (incl. `*0`) with guard, lexicographic `< <= > >=`, append/pop(idx)/insert
  (clamps)/remove/index(start,end)/count/extend/copy(shallow, independent)/clear/reverse, `apply`
  (empty, missing-fn throw), `_eq_`-driven membership/index/count/remove on instances (non-mutating).
- sort: ascending, empty, single, strings, floats, reverse, key, key+reverse, **stability** (incl.
  under reverse and a 1000-element random tie stress), multi-key list keys, mixed int/float numeric
  order, un-orderable-type throw, list-vs-non-list throw. `sorted()` builtin parity.
- Set: dedup, len/membership, add/discard(no-error)/remove/contains/pop/clear, union/intersection/
  difference/symmetricdifference/issubset/issuperset/isdisjoint (+ missing-arg throws), operator
  algebra (`-`, `< <= > >=` with Set rhs required), apply (collision-collapse), equality by membership,
  empty operands.
- Dict: get/set/`in`(keys-not-values)/len, missing-key throw, unhashable-key throw, keys/values/items,
  get/pop/setdefault (+ defaults + kwargs + missing throws), remove(absent throw)/popitem(empty throw),
  update (Dict / pairs / bad-pair throw / non-iter throw), copy(shallow), clear, apply(over values),
  order-independent equality, multi-key subscript rejected.
- Hashing: Int/Float hash-consistency with `==` (integral Float hashes as the int64; ±0.0 collapse;
  ±inf distinct; NaN write-only keys), `0` vs `False` distinct keys, Bytes hashable as key,
  user-class `_hash_` opt-in + `_eq_` consistency (`class_hash.ki`), Dict/Set reentrant `_eq_`/`_hash_`
  guard (`test_audit_hardening.cpp`, ASan-stressed).
- Stringify: cyclic list `[...]`, deep-acyclic throw, repr-in-container for Strings/Bytes.

**Gaps (no test found):**
1. **List value-search reentrancy** — the A09-1 UAF: no test drives `remove`/`index`/`count`/`in`
   with an `_eq_` that mutates the list. Dict/Set have such a test; List does not.
2. **NaN in ordering** — no test sorts / `min`s / `max`es a NaN-containing list (A09-2).
3. **Unhashable in membership** — `[] in aSet` vs `[] in aDict` asymmetry (A09-3) is untested; the
   existing "unhashable key throws" test only covers subscript `d[[]]`, not `in`.
4. **Large Dict/Set stress** — only List sort has a 1000–2000-element stress; no bulk
   insert/lookup/delete correctness test for Dict/Set (bucket integrity, count invariant under churn).
5. **Mutation-during-for-loop** — the snapshot semantics (`for x in xs: xs.append(...)` iterates a
   frozen copy) are relied upon by `iterate()` returning a copy but never asserted.
6. **Dict/Set bucket reclamation / churn** (A09-4) — no soak test; empty-bucket accumulation unobserved.
7. **`extend`/`update` self-argument** — `xs.extend(xs)` and `d.update(d)` are safe (snapshot) but
   untested; a good regression pin.
8. **Set.pop determinism/exhaustion** — `pop` down to empty then throw is only single-element tested.
9. **Cross-type numeric key unification** — `d[1]` then `d[1.0]` resolving to the SAME entry (a direct
   consequence of `1 == 1.0` + matching hash) is not asserted; the `r4` comment even mislabels
   "1.0 vs 1" as distinct keys, which they are not.

## Notes verified as CORRECT (no action)

- `sliceIndices` (native.hpp 46-79) is count-driven and overflow-safe at INT64_MIN step / huge step —
  no OOB, no non-termination. Slice/index negative handling is correct.
- `List.insert` clamps both directions; `List.pop(idx)` negative + range check correct.
- GC rooting in `sort`/`apply`/`sorted`/`Dict.items`/`Dict.keys`/`Dict.popitem`/`Set` union family is
  present and correct (snapshots + `RootScope`); List `==`/`Dict ==` re-fetch the vector via the stable
  object reference (not a cached vector ref), so they are NOT vulnerable to the A09-1 realloc — only the
  value-search methods cache the reference.
- `FloatVal::hash` range `[-2^63, 2^63)` matches `intFloatEqual` exactly; Int/Float/`==`/hash triad is
  consistent. `EqualsGuard` (thread-local shared depth counter, cap 1000) bounds both `kiEquals` and
  `kiLessThan` recursion, so cyclic/deeply-nested `==`/`<`/sort throw instead of overflowing.
- Mutable containers (List/Set/Dict) correctly are NOT hashable, so they cannot be Dict/Set keys.
- Dict/Set `ProbeScope` reentrancy guard is sound and tested.
