# v1.15 audit — Collections (List/Set/Dict) internals + Bytes

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
