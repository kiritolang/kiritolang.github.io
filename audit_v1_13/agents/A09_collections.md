# A09 — Collections Audit (List / Set / Dict / Array + hashing)

Auditor: A09 (read-only static audit). Scope: `src/kirito/collections.hpp`,
`src/kirito/hashing.hpp`, and the List/Set/Dict/Array method + operator + iteration
implementations in `src/kirito/runtime.hpp`. Existing tests:
`tools/tests/unit/test_list_ops.cpp`, `test_sort.cpp`, `test_collections_deep.cpp`,
`tools/tests/scripts/` golden scripts.

Status: IN PROGRESS.

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
