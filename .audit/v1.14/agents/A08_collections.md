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

Status: IN PROGRESS.

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

