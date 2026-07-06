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
