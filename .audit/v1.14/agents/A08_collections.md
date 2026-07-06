# A08 — Collections Audit (v1.14) — List / Set / Dict / Array + hashing

Auditor: A08 (read-only static audit). Scope: `src/kirito/collections.hpp`,
`src/kirito/hashing.hpp`, and List/Set/Dict/Array method + operator + iteration
implementations in `src/kirito/runtime.hpp`.

NOTE: Task named `list_value.hpp`/`set_value.hpp`/`dict_value.hpp`/`array_value.hpp` —
these do NOT exist. Actual layout: `collections.hpp` (Val classes) + method surface in
`runtime.hpp` + `hashing.hpp`. Matches v1.13 layout.

Prior v1.13 findings (A09): A09-1 List value-search UAF via reentrant `_eq_` (High),
A09-2 NaN sort UB, A09-3 unhashable-in-dict-vs-set asymmetry, A09-4 empty-bucket leak,
A09-5 subset family unrooted, A09-6 DRY + phantom Array kind. Checking which merged.

Status: COMPLETE. 1 HIGH (new, confirmed crash), 4 low, 1 dry, 1 coverage-gap.

---

## Findings

### A08-1: `Set.clear()` / `Dict.clear()` bypass the ProbeScope guard → reentrant clear during a probe is a double-free / heap corruption (CONFIRMED CRASH)

- **severity**: HIGH (memory safety — confirmed double-free / heap corruption from pure Kirito code, crashes even the non-ASan debug build)
- **location**: `src/kirito/runtime.hpp` Set `clear` (914-920) and Dict `clear` (784-790); also Set `pop` (921-932). Contrast with Set/Dict `add`/`set`/`remove`/`discard` which all begin with `if (probing_) throw KiritoError("... changed size during a ... comparison")` (collections.hpp 144/217/255, runtime.hpp 882/905).
- **category**: bug / reentrancy / iterator-and-value invalidation
- **description**: The Dict/Set ProbeScope defense (collections.hpp 112-126) assumes every MUTATING op refuses to run while `probing_` is set. But `clear()` (both containers) and `Set.pop()` were never wired into that guard — they neither check `probing_` nor are protected by a `ProbeScope`. `clear()` calls fum's `destroy_all_values()` (hash_table.hpp 866-877), which DESTRUCTS the per-hash `std::vector<Handle>` bucket values and clears `iteration_`. A probe (`find`/`contains`/`add`/`set`/`remove`, or `SetVal::equals` via `o.contains`) holds a live C++ reference `it->second` / `const Bucket& bucket` into exactly that vector and iterates it in `probeBucket` (`for i < bucket.size()`). A user `_hash_`/`_eq_` invoked mid-probe (InstanceValue::hash/equals run arbitrary Kirito code via `activeVM()`, runtime.hpp 1777/1804) that calls `container.clear()` destroys the bucket vector out from under the loop → the next `bucket.size()`/`bucket[i]` reads a destructed vector → double-free / use-after-free / heap corruption.
- **failure-scenario** (both abort with `free(): double free detected in tcache 2`, exit 134, on `build-debug/ki` — no ASan needed):
  ```
  var s = Set()
  class K:
      var _init_ = Function(self, n): self.n = n
      var _hash_ = Function(self): return 0            # collapse into one bucket
      var _eq_ = Function(self, other):
          s.clear()                                    # destroys the bucket being probed
          return self.n == other.n
  var i = 0
  while i < 40: s.add(K(i)); i = i + 1
  var r = K(5) in s                                    # CRASH: double free
  ```
  Same with `d = {}` + `d.get(K(5), -1)` (Dict path). Also reachable via `set1 == set2` (kiEquals has no Set==Set fast-path, so it routes to the live-iterating `SetVal::equals` at runtime.hpp 1633; a clearing `_eq_` there dangles the range-for over `this.buckets` — p18 corrupted silently in debug, len went 40->0 mid-compare).
- **why v1.13's A09-1 fix didn't cover this**: A09-1 hardened the *List* value-search methods (snapshot). The Dict/Set ProbeScope was believed complete, but it only guards add/set/remove/discard; `clear`/`pop` are the untouched holes. This is a genuinely NEW angle.
- **proposed-test**: ASan CTest running the two snippets above (Set `in` + Dict `get`) and `set1 == set2` with a clearing `_eq_`; assert a clean catchable "changed size during a comparison" error, never a crash. Mirror `test_audit_hardening.cpp`.
- **proposed-fix**: Add `if (probing_) throw KiritoError("Set changed size during a value comparison")` (Dict: "Dict changed size during a key comparison") at the top of `clear` and `Set.pop`, exactly as add/remove/discard do. (Deeper: also give `SetVal::equals` its own `ProbeScope` on `this`, or add a Set==Set snapshot fast-path in kiEquals mirroring the Dict `pairs()` snapshot at runtime.hpp 1621, so `==` is symmetric with the operator paths.)
- **confidence**: CERTAIN — reproduced as a hard double-free abort on the shipped debug binary.

### A08-2: NaN total-order fix for List `sort`/`min`/`max` (v1.13 A09-2) is IN CODE but UNTESTED

- **severity**: coverage-gap
- **location**: `src/kirito/runtime.hpp` kiLessThan (393-410, the NaN branch imposing "NaN sorts largest"); tests `tools/tests/unit/test_sort.cpp`, `test_collections_deep.cpp`.
- **category**: coverage-gap
- **description**: v1.13 A09-2 flagged NaN sorting as UB; the fix landed (kiLessThan now returns a strict-weak-order total: `isNan(y) && !isNan(x)`, NaN after +inf). CONFIRMED working: `[3.0, nan, 1.0, 2.0, inf].sort()` -> `[1.0, 2.0, 3.0, inf, nan]`, `min([1.0,nan,2.0])==1.0`, `max(...)==nan`. But NO test pins it: `test_sort.cpp` has no NaN case; `test_collections_deep.cpp` tests NaN only in *value-search* (count/index/remove), not ordering. The only NaN-sort tests in the tree are for `tensor` (r6_tensor.ki:382) and `median` (r7_regressions.ki:94) — the List/`sorted()`/`min`/`max` scalar path is unpinned, so a regression in kiLessThan would go unnoticed.
- **proposed-test**: In `test_sort.cpp` or `spec_collections.ki`: assert `[3.0, math.nan, 1.0].sort()` ends with nan, `min`/`max` with a NaN element, and a mixed Int/Float+NaN list — a *defined* outcome each.
- **confidence**: High (grep-confirmed absence).

### A08-3: Unhashable membership asymmetry (v1.13 A09-3) persists — `[] in dict` throws, `[] in set` returns False

- **severity**: low (+ coverage-gap)
- **location**: `DictVal::contains` -> `find` -> `requireHashable` throws (collections.hpp 152-154, runtime.hpp 1649); `SetVal::contains` returns `false` on non-hashable (collections.hpp 263-265, runtime.hpp 1652); `ListVal::contains` compares structurally.
- **category**: bug / inconsistency (unchanged from v1.13; still no test)
- **description/failure-scenario**: CONFIRMED still divergent: `[] in {1:2}` -> throws `unhashable type 'List'`; `[] in {1,2}` -> `False`. Two hash-bucketed containers with parallel APIs disagree. Untested (all membership tests use hashable operands).
- **proposed-fix**: Pick one policy for both. Simplest: `DictVal::contains` short-circuits `return false` for a non-hashable key (leave subscript `d[[]]` throwing via `find`). proposed-test: pin both in spec_collections.ki.
- **confidence**: High (asymmetry certain; which side is "right" is a maintainer call).

### A08-4: Empty hash buckets never reclaimed on delete (v1.13 A09-4) persists

- **severity**: low / perf
- **location**: Dict `remove` (collections.hpp 213-227 — `bucket.erase(...)` but no `buckets.erase(it)` when empty); Set `remove`/`discard`/`pop` (runtime.hpp 887/910/927).
- **category**: weak spot / slow leak + perf
- **description**: Unchanged from v1.13. Grep confirms the ONLY erase is `collections.hpp:224 bucket.erase(bucket.begin()+i)` — never the map entry. Distinct-hash insert-then-delete churn grows the bucket map without bound (`buckets.size()` only shrinks on `clear()`); `Set.pop` linear-scans buckets for the first non-empty, degrading as empties accumulate. `len` stays correct. NOTE: fum's map has a free-list/arena (hash_table.hpp), so real-world impact may be smaller than v1.13 assumed — but the empty `std::vector<Handle>` node still lingers. A churn probe (5000 add+remove) kept `len==0` correctly; memory bound not observable from Kirito.
- **proposed-fix**: `buckets.erase(it)` when the bucket empties in remove/discard/pop.
- **confidence**: High on mechanism; Low on impact.

### A08-5: `issubset`/`issuperset`/`isdisjoint` still don't root the iterated `other` (v1.13 A09-5) persists

- **severity**: low (latent)
- **location**: `SetVal::getAttr` subset/superset/disjoint branch (runtime.hpp 967-987) builds `SetVal otherSet;` with NO `RootScope`, unlike the union family (938-946) which does `for (Handle e : other.value()) rs.add(e);`.
- **category**: weak spot / GC-safety inconsistency (unchanged from v1.13)
- **description**: Safe for builtin elements (their hash/eq allocates nothing). Only unsafe if `other` yields freshly-allocated instances whose `_hash_`/`_eq_` triggers a collecting allocation between `otherSet.add` calls — hard to reach, but the asymmetry with the union family is a real code smell.
- **proposed-fix**: Root `other.value()` in this branch too; DRY a shared "materialize other into a rooted SetVal" helper (also removes the union-family duplication).
- **confidence**: Medium safe today; inconsistency certain.

### A08-6: Phantom `ValueKind::Array` (v1.13 A09-6) persists — dead defensive branches

- **severity**: dry
- **location**: `ValueKind::Array` referenced in runtime.hpp 264/419/420/447/475, value.hpp 165, stdlib_json.hpp 281/321, stdlib_tensor.hpp 1570 — but NO `Object::kind()` anywhere returns `ValueKind::Array` (grep of `return ValueKind::Array` = 0 hits). There is no `ArrayVal` class; `ListVal::kind()` is hard-coded to `List`.
- **category**: DRY / dead code
- **description**: The "internal Array — same value model as List" in CLAUDE.md does not exist as a distinct runtime type; every `|| ...==Array` check is an unreachable branch. Either wire up the intended Array type or drop the phantom kind + its dead checks.
- **confidence**: High.

### A08-7: List `remove` erases by snapshot index into the live list — can delete the wrong element if `_eq_` mutates mid-search

- **severity**: low (non-crash semantic wart; the A09-1 hardening traded a UAF for this)
- **location**: `ListVal::getAttr` `remove` (runtime.hpp 608-622): finds the match index `i` in the rooted snapshot, then `auto& e = self_list(...).elems; if (i < e.size()) e.erase(e.begin()+i);`.
- **category**: weak spot / correctness-under-reentrancy
- **description**: Memory-safe (bounds-checked), but if the searched value's `_eq_` mutates THIS list during the search (insert/remove shifting positions), the erase targets snapshot index `i` in the now-different live list — deleting an element that isn't the matched one, or silently no-op'ing when `i >= e.size()`. Pathological but a latent surprise. (index/count are read-only so they're only "stale", not destructive.)
- **proposed-fix**: After finding the match, re-locate it in the live list by identity/equality before erasing, or document that mutation-during-remove is unsupported.
- **confidence**: Medium (reasoned from code; not weaponized).

**A08-1 reachability note**: broadened — even a plain `s.add(K(5))` crashes (double-free), because add's own dedup probe runs the element `_eq_` which calls `clear()`. So the hole is reachable through *every* probing op (add/set/`in`/get/remove/`==`), not just `==`. The fix (guard `clear`/`pop`) closes all of them at once.

---

## Coverage assessment (v1.14 delta)

Verified via `test_audit_v113.cpp` (A09-1 List-search UAF now gated, ASan), `test_collections_deep.cpp`
(NaN value-search, slice bounds, Dict pop/setdefault), `test_sort.cpp`, `test_collections.cpp`,
`test_list_ops.cpp`, `test_hash.cpp`, `spec_collections.ki`, `r4_collections.ki`, `class_hash.ki`,
`spec_iter_mutation.ki`, `spec_audit_hardening.ki`.

**Confirmed working (probed live on build-debug/ki):**
- Reentrant `_hash_` during Dict insert (mutates before ProbeScope — safe, hash computed pre-bucket-ref); reentrant `_eq_` during insert -> clean "Dict/Set changed size" guard.
- Set `apply` fn->unhashable throws cleanly, original set intact; Dict/Set/List apply snapshot-protected.
- NaN List sort/min/max (total order, NaN last) — WORKS but UNTESTED (A08-2).
- Self-cycle List sort -> clean order-error; self-cycle List `<` -> EqualsGuard depth error (no overflow).
- Mixed-type sort/`<` -> clean throw. `list*0`/`list*-1` -> `[]`; huge repeat -> "repeated List too large".
- Slice: zero-step throws, negative step, OOB tolerant. **Slice ASSIGNMENT `xs[1:3]=...` -> compile error "invalid assignment target"** (unsupported — not a bug, but a doc/coverage note: no slice-assign).
- Set algebra edges: `a<=a`/`a>=a` True, `a<a`/`a>a` False (proper), `a-a`={}, empty subset/disjoint.
- Dict empty ops: popitem/pop throw, get/setdefault defaults correct; `extend(self)`/`update(self)` safe.
- Churn (5000 add+remove) keeps `len` correct; 3000-element Set membership correct.
- Numeric key unification: `d[1]`==`d[1.0]` (one entry); `1==1.0` True. Bool distinct: `True==1` False, `len({1,True})==2`, `len({0,False})==2` — hash/eq consistent (Bool is its own type).
- Dict/Set iteration is INSERTION-ORDERED (fum stable-arena `iteration_` vector) — `{z,a,m,b}.keys()` preserves order.

**Gaps flagged:**
1. A08-1 clear/pop reentrancy — NO ASan test for a clearing `_hash_`/`_eq_` (the exact crash). HIGH priority to add.
2. A08-2 NaN List sort/min/max — untested.
3. A08-3 unhashable `in` set-vs-dict asymmetry — untested + inconsistent.
4. No test for slice-assignment rejection (minor).
5. No test pinning Dict/Set insertion-order guarantee (relied on, unasserted).

## Notes verified CORRECT (no action)
- Dict `==` uses `da.pairs()` snapshot (kiEquals 1621) — safe from key/value-`_eq_` realloc.
- List `==`/value-search rooted+snapshot (A09-1 fix intact); List `remove` bounds-checked (see A08-7).
- InstanceValue hash/equals root receiver+method (runtime.hpp 1786-1788) before the call.
- ProbeScope correctly guards add/set/remove/discard; probeBucket's no-`is`-before-`==` NaN-write-only-key is a documented design choice.
- fum map node arena is address-stable across rehash, so cross-bucket inserts during iteration don't dangle the *node*; only same-bucket vector realloc or `clear()`/`destroy_all_values` does (-> A08-1).

## Summary

7 findings: **1 HIGH** (A08-1 clear/pop reentrancy double-free — CONFIRMED crash, NEW), 4 low
(A08-3 unhashable asymmetry, A08-4 empty-bucket leak, A08-5 subset unrooted, A08-7 remove stale-index),
1 dry (A08-6 phantom Array), 1 coverage-gap (A08-2 NaN sort untested) — plus A08-3 doubles as a
coverage-gap. A09-1 (List UAF) and A09-2 (NaN sort UB) from v1.13 are FIXED in code; A09-3/4/5/6 persist.

Status: COMPLETE.
