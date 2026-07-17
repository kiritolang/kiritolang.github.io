# A03 — GC, arena, rooting, write barrier (v1.15.1)

Scope: the whole codebase, hunting the two A19 bug shapes:
1. **Unrooted temporary** — a value materialised outside the arena / before its owner is reachable.
2. **Missing write barrier** — a raw write to a container reached via `deref()` (may be old-gen).

Plus: generational cadence, `resetRemembered`, promotion, `sweepYoung`, pin/unpin refcounting,
`PinnedHandle`, `RootScope`/`tempRoots_`, aux root regions, `liveCount()` retarget ratchet.

Method: probe `./build-debug/ki --gc-threshold 1` with values OUTSIDE the intern range
(`kSmallIntLo=-256`, `kSmallIntHi=256`), omitted optional args, and deferred use of results.

Status: IN PROGRESS

---
## Static sweep — results

### Shape 2 (missing write barrier): swept, ONE non-finding
Grep for raw writes to a `deref()`-reached container across all of `src/kirito/*.hpp`:
`\.elems\.(push_back|emplace_back|insert|assign|resize)|\.attrs\[|\.methods\[|\.members\[`
→ exactly **one** hit outside `arena.hpp`: `stdlib_serde.hpp:603` `inst.attrs[...] = av`.
It is **explicitly barriered** on the line above (`gcWriteBarrier(vm.arena(), &inst, av)`). Not a bug.
`json.hpp`'s `array()` (the A19-2 site) now uses the barriered `append(arena, h)`. **No second
A19-2 instance exists.** (parser.hpp `->elems.push_back` hits are AST nodes, not arena objects.)

### Shape 1 (unrooted temporary): all 72 `make_unique<ListVal|DictVal|SetVal|InstanceValue>` sites reviewed
Every site either (a) roots each element as it is materialised (`rs.add(...)`), or (b) pushes handles
that are already arena-reachable from a live root (a live dict's `k`/`v`, the operand stack, a
`rootedIterate` result already in `rs`). Reviewed in detail and found correct:
`value.hpp` List/Dict/Set ctors + `Dict::set`; `runtime.hpp` divmod / enumerate / zip / map / filter /
sorted / dict keys,values,items,apply,popitem / set algebra; `bytecode_vm.hpp` BuildList/BuildSet
(elements live on `stack_`, an aux root region, and `stack_` is not resized until after the alloc);
`dispatcher.hpp` `packArgs`; `stdlib_tensor.hpp` g_split / nonzero / tolist / shape;
`stdlib_matrix.hpp` + `stdlib_complex.hpp` getItem-row / shape / compare; `stdlib_random.hpp`
choices / sample; `stdlib_io.hpp` readlines; `stdlib_json.hpp` object/array.

`bytes.hpp:294` (`Bytes.apply`) passes an **unrooted** `vm.makeInt(c)` to an allocating user callback —
safe **only** because a byte is 0..255, inside the intern range; the code carries a comment saying so.
Correct today, but it is the exact A19-1 shape held safe by an invariant one edit away from breaking.
Noted as a latent-hazard, not a finding.

## Live probes — the A19 fix set holds

All under `./build-debug/ki --gc-threshold 1`, values outside the intern range, results stored and
asserted AFTER heavy churn.

- **p1** (`json.loads` nested / `enumerate` >255 / map / filter / zip / sorted(key) / divmod /
  split / join) → `P1 OK 1482`
- **p2** (`apply` on List/Dict/Set/String/Bytes, `min`/`max(key=)`, `regex.sub(callable)`,
  `regex.groupdict`, `functools.reduce`) → `P2 OK 2269`
- **p3** (`Tensor/Matrix/ComplexMatrix.shape()` with dim 300, **every `.compare()` with the
  `rel_tol`/`abs_tol` defaults OMITTED** — Integer/Float/Matrix/ComplexMatrix/Tensor/Complex,
  `tensor.nonzero`, `tolist`, `tensor.apply`, matrix/cmatrix row indexing) → `P3 OK 2252`

No `dangling handle` and no value corruption on any of these. The v1.15 A19-1/A19-2 fixes are
verified live and complete for the sites they name.

---
## FINDINGS

### A03-1: `Dict.update(iterable)` hands out a dangling handle — the iterate result is never rooted  [severity: HIGH] [confidence: confirmed]

- **Location**: `src/kirito/runtime.hpp:816-823` (`DictVal::getAttr`, the `update` non-Dict branch)
- **What**: the branch calls raw `.iterate(vm)` and roots **neither** the outer element vector (`*it`)
  nor each pair's elements (`*pit`), then calls `DictVal::set`, which runs a user `_hash_`/`_eq_` —
  arbitrary Kirito code that allocates. `InstanceValue::iterate` (runtime.hpp:1944) roots its result in
  a `RootScope` that **dies when it returns**, so when `_iter_` returns a *freshly built* container,
  nothing roots it or its elements once `iterate()` has returned. The next allocation collects the
  whole outer list, and the loop then `deref`s a stale handle.
  This is precisely the shape `rootedIterate` (runtime.hpp:2871) exists to prevent, and which the
  **set-algebra family already guards** (`union`/`intersection`/`difference`/`symmetricdifference`
  and `issubset`/`issuperset`/`isdisjoint` each do `for (Handle e : other.value()) rs.add(e);`,
  citing A08-5). `update` is the sibling that was missed.

- **Repro** (`/tmp/p10.ki`):
```kirito
var io = import("io")
class Item:
    var _init_ = Function(self, v):
        self.v = v
    var _hash_ = Function(self):
        var pad = "allocate-" + String(self.v)   # ALLOCATES -> triggers a GC
        return self.v
    var _eq_ = Function(self, o):
        return self.v == o.v
    var _str_ = Function(self):
        return "Item(" + String(self.v) + ")"
class Bag:
    var _iter_ = Function(self):                 # a FRESH list, unrooted once iterate() returns
        return [[Item(1000), "v1000"], [Item(2000), "v2000"], [Item(3000), "v3000"]]
var d = {}
d.update(Bag())
io.print("update ->", d)
```
  Real output:
```
--- default threshold ---
update -> {Item(1000): 'v1000', Item(2000): 'v2000', Item(3000): 'v3000'}
--- gc-threshold 1 ---
Traceback (most recent call last):
  File "/tmp/p10.ki", line 20, in <module>
/tmp/p10.ki:20:9: error: dangling handle (stale generation)
```
  Correct at the default cadence, **dangling under the soak** — the classic A19-1 signature (a GC has
  to land in a few-instruction window, so it is rare-but-real at a normal threshold, not impossible).

**Stronger repro — NO custom dunders needed** (`/tmp/p14.ki`). Iterating each 2-char String pair
itself allocates the fresh 1-char Strings, and *that* is the allocation that collects the outer list:
```kirito
var io = import("io")
class Bag:
    var _iter_ = Function(self):
        var out = []
        for i in range(8):
            out.append(chr(65 + i) + chr(97 + i))   # COMPUTED, so not const-pool-pinned
        return out
var d = {}
d.update(Bag())
io.print("update ->", d)
```
```
--- default ---
update -> {'A': 'a', 'B': 'b', 'C': 'c', 'D': 'd', 'E': 'e', 'F': 'f', 'G': 'g', 'H': 'h'}
--- gc1 ---
/tmp/p14.ki:11:9: error: dangling handle (stale generation)
```

**Masking note (important, generalises beyond this finding):** the pairs must be **computed**. A
literal (`return ["ab", "cd"]`) does NOT reproduce, because **String literals are pinned for the VM's
lifetime in the bytecode constant pool** (`KiritoVM::pinConst` / `bytecodeConsts_`, a permanent GC
root). This is a **second masking mechanism alongside small-int interning** — and the v1.15 lesson
("`--gc-threshold 1` is only as good as the values the tests use") applies to it just as much. A test
written with literal strings is immune to this whole bug class. Worth adding to the round's README
next to the small-int note.

- **Impact**: any `d.update(x)` where `x` is a user class whose `_iter_` returns a freshly built
  container (the natural way to write `_iter_`) and the loop allocates — either via an allocating
  `_hash_`/`_eq_` on the key, or simply because the pairs are Strings/Bytes (whose `iterate`
  allocates).
  Fails as a `dangling handle` throw, or — since a freed slot can be **reused** rather than caught by
  the generation check — as a silently wrong key/value.
**Reachable at the DEFAULT GC cadence — this is NOT a soak-only artifact.** Unlike every A19-1 site
(which needed `--gc-threshold 1`), this one fires with **no flags at all**. A *small* fresh `_iter_`
list is never promoted during construction, so it stays young and a plain minor reclaims it
(`/tmp/p19.ki`, 200k `update()` calls, default cadence):
```
THREW at iter 2973 : dangling handle (stale generation)
total failing iterations: 16 of 200000
gc-stats: minor=257 (1928.26 ms)  major=11 (572.115 ms)  live=218657  young=17841  capacity=233691
```
≈1 failure per 12,500 calls — ~6% of the 257 minors landed inside `update`'s loop. It also fires at
`--gc-threshold` 1, 2, 3, 50 and 100 (thresholds 5–21 happen to miss the window). **This raises the
severity above the A19-1 class: it is reachable in ordinary production, not only under the soak.**

**Differential sweep — `update` is the ONLY affected API.** The same `Bag` (whose `_iter_` returns a
freshly built container) fed to **27** iterable-consuming APIs, output diffed between the default
cadence and `--gc-threshold 1` (`/tmp/p15.ki`):
```
=== DIFF default vs gc1 ===
6c6
< dict.update => {'A': 'a', 'B': 'b', ... 'H': 'h'}
---
> dict.update => THREW: dangling handle (stale generation)
```
Every other API is identical under both: `List/Set/Dict(iterable)`, `list.extend`, the six
`set.union/intersection/difference/symmetricdifference/issubset/isdisjoint`, `sorted`, `min`, `max`,
`map`, `filter`, `enumerate`, `zip`, `all`, `any`, `reversed`, the `for` loop, unpacking, `join`,
`random.choice/choices/sample`, `math.prod`. So the fix is genuinely one site, and the surrounding
family already demonstrates the correct idiom.

- **Proposed fix**: root both levels, reusing the existing single-sourced helper — replace the raw
  `o.iterate(vm)` with `rootedIterate(vm, a[0], rs, "update expects a Dict or an iterable of
  [key, value] pairs")` under a `RootScope rs(vm)`, and root each `*pit` element (`rs.add`) before
  the `set`. Matches the set-algebra family exactly; breaks no documented contract.
- **Proposed test**: `tools/tests/scripts/` — a `.ki` golden asserting the p10 repro's dict contents,
  run under the soak; plus a C++ pin in `tools/tests/unit/test_gc_generational.cpp` at
  `setGcThreshold(1)`, which must FAIL on the unfixed build.
