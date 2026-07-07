# v1.12.1 (audit loop) — Collections (List/Set/Dict) internals + Bytes

Subsystem: `src/kirito/collections.hpp`, `src/kirito/bytes.hpp`.
Probe binary: `./build-debug/ki`.

## LOG
- Read collections.hpp + bytes.hpp fully; read supporting runtime.hpp (List/Set/Dict methods, set algebra,
  sliceIndices, numeric equals/hash), builtins.hpp (None/Bool/Int/Float/Str hash/equals).
- VERIFIED OK (by design / robust):
  - Cross-type key identity: 1 == 1.0 collapse (equal + hash-equal); True distinct from 1 (equal returns
    false, though hash(True)==hash(1) — allowed collision). {1,1.0}->1 elem, {1,True}->2 elems. GOOD.
  - Dict iteration order NOT insertion-stable across delete (fum swap-on-erase reorders). BUT docs
    (09-types.md:274) explicitly say Dict is unordered — BY DESIGN, not a bug.
  - Reentrancy probing_ guard: reentrant _eq_ mutating same Set/Dict throws "changed size during ...". GOOD.
  - NaN write-only key; inf works; 0/0.0/-0.0 collapse (== + hash agree). GOOD, documented.
  - Unhashable keys rejected in Dict set/get; Set add. GOOD.
  - Bytes construction (List/int-n/String/Bytes/Set/range iterable), encode/decode (utf8/latin1/ascii,
    invalid utf8, cp>cap, surrogate reject via validUtf8), hex/fromhex (odd/non-hex), slice/index/neg,
    `*`(neg->empty, huge guarded)/`+`/ordering/hashing/set-dedupe. ALL correct.
  - sliceIndices: count-driven, INT64_MIN step safe, zero-step throws. Robust.
  - List pop/insert/remove/index/count: negative idx, boundary, rooted snapshots vs reentrant _eq_. Robust.
  - list `*`/`+` overflow guards (kMaxRepeat), range guards. Robust.
  - Set algebra (union/inter/diff/symdiff/subset/superset/disjoint) over fresh-handle iterables
    (String/Bytes/range) with GC-rooting (RootScope rs). Correct results; rooting looks sound.
  - Self-referential str cycle-guarded; self-ref equals hits catchable recursion guard. GOOD.
  - Set operators `-`/`<`/`<=`/`>`/`>=` require Set on RHS (documented); methods take any iterable.

## FINDINGS

### F1 [MED/SUSPECT] `x in <Set>` returns False for unhashable x, but `x in <Dict>` throws — inconsistent
- where: src/kirito/collections.hpp:275-283 (SetVal::contains) vs DictVal::find (collections.hpp:152-161, via DictVal::contains runtime.hpp:1650)
- repro:
  ```
  [1] in Set()   # => False
  [1] in {}      # => THROW: unhashable type 'List'
  ```
- actual: Set membership of an unhashable value silently returns False; Dict membership throws.
  expected: consistent behavior. Python raises TypeError for BOTH. Given Kirito's stated principle of
  rejecting unhashable keys with a clear error (and Dict already does), Set silently returning False masks
  a likely programming bug and diverges from Dict.
- note: SetVal::contains(arena,Handle) returning false is deliberately relied on by internal set algebra
  (issubset etc.) where you don't want to throw — so the fix should distinguish the user-facing `in`
  (SetVal::contains(KiritoVM&,...) at runtime.hpp:1653) from the internal helper, OR accept it as by-design.
  Same asymmetry in remove/discard: Set.remove throws "unhashable type", Dict.remove(unhashable) reports
  "key not found". Flagging as SUSPECT for maintainer decision.
- fix idea: make the user-facing `x in set` throw on unhashable (mirror Dict), keeping the internal
  arena-only contains lenient; or document the intentional divergence.

### F2 [HIGH] Heap-use-after-free: reentrant mutation during Dict/Set stringify (str() not probing_-guarded)
- where: src/kirito/collections.hpp:178-192 (DictVal::str) and 314-326 (SetVal::str)
- root cause: `equals()` and every mutator take a `ProbeScope guard(probing_)` so a reentrant
  `_hash_`/`_eq_` that mutates the SAME container throws instead of reallocating a live bucket. But
  `str()` sets NO probing_ guard, yet it CAN run arbitrary user code: a contained value that is an
  instance with `_str_` (InstanceValue::str, runtime.hpp:1914) executes during `stringifyChild`. That
  `_str_` can call `d[...] = v` / `s.add(...)`, which reallocates the bucket `std::vector` (or the
  `fum::unordered_map`) that `str()`'s range-for is iterating -> UAF / heap corruption.
- repro (Dict), CONFIRMED under build-asan (heap-use-after-free at collections.hpp:186):
  ```
  var io = import("io")
  var d = {}
  class Key:
      var _init_ = Function(self, n): self._n = n
      var _hash_ = Function(self): return 5          # force one shared bucket vector
      var _eq_ = Function(self, o): return self._n == o._n
      var _str_ = Function(self): return "K" + String(self._n)
  class Bomb:
      var _str_ = Function(self):
          var i = 100
          while i < 400:
              d[Key(i)] = i                          # grow the hash-5 bucket mid-stringify
              i = i + 1
          return "boom"
  d[Key(0)] = Bomb()
  d[Key(1)] = 1
  d[Key(2)] = 2
  var s = String(d)                                  # ASan: heap-use-after-free
  ```
  Set version (build-asan, UAF at collections.hpp:319): same shape with `g.add(E(i))` inside a
  contained element's `_str_` (all elems `_hash_`=7 into one bucket).
- actual: AddressSanitizer heap-use-after-free (freed by DictVal::set's vector _M_realloc_insert,
  read by DictVal::str). On the plain debug build it silently "survives" printing garbage (UB).
- expected: like a reentrant mutation during a key comparison, this should throw a clean catchable
  "Dict/Set changed size during stringify" error (or stringify a snapshot), never corrupt the heap.
- fix idea: wrap the DictVal::str / SetVal::str body in `ProbeScope guard(probing_)` (mutators already
  reject when probing_ is set — gives the same catchable error as equals). Alternatively stringify a
  handle SNAPSHOT (`pairs()`/`items()` copy the handles before any _str_ runs). Note: ListVal::str is
  NOT affected — it re-indexes `elems[i]` with `i < elems.size()` each iteration and copies the Handle
  before the call, so append/clear can't dangle it (worst case: a clear ends the loop early).
