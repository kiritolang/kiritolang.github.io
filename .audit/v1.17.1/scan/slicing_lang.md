# Audit: basic indexing / slicing language machinery (v1.17.1)

Auditor: correctness/memory scan. Method: read the full slicing path, then reproduce every claim on
`build-bin/ki-release` (behavior) and `build-bin/ki-asan` (GC/UBSan, `ASAN_OPTIONS=detect_leaks=0`,
`ulimit -s 262144`, `KIRITO_GC_THRESHOLD=1`, fresh non-interned big-int bounds). No source/tests edited.

## Result

**No bugs found.** Zero CONFIRMED or SUSPECTED defects. Every hunt item in scope was probed on a real
binary and behaves correctly. One deliberate design divergence from Python is noted below (not a bug).

---

## Design note (not a defect) — multi-axis subscript delivers SEPARATE args to `_getitem_`

A multi-axis subscript on a user class delivers each axis as a **separate positional argument**, not as a
packed tuple the way CPython's `__getitem__` receives `obj[1, 2:4]` as a single `(1, slice(2,4))` tuple.

- `src/kirito/class_value.hpp:151` + `src/kirito/runtime.hpp:2038-2049` (`InstanceValue::getItem`/`slice`)
  and `src/kirito/parser.hpp:684-698`: single-axis slice → one `Slice` arg; multi-axis → N keys → N args.
- Reproducer: `m[1, 2:4]` requires `_getitem_(self, a, b)`; with `_getitem_(self, key)` it raises
  `'_getitem_' takes 2 positional argument(s) but 3 were given`. CONFIRMED, and it matches the in-code
  comment ("Multi-axis `x[a, b:c]` already flows through getItem as mixed keys").

This is internally consistent and documented, but (a) diverges from Python and (b) there is no varargs
support in Kirito function params (`Function(self, *keys)` is a parse error), so a class cannot write one
`_getitem_` that accepts a *variable* number of axes. Not a correctness bug; flagging for awareness /
possible doc mention. No action required by scope.

---

## GC hazards — all sound (CONFIRMED)

- `SliceVal::children()` (`src/kirito/builtins.hpp:195-199`) reports all three handles (`start_/stop_/step_`),
  each behind a defensive `.slot` guard. Bounds are never null-handle in practice (`MakeSlice`/`slice()`
  default push `None`, which has a valid slot).
- `Op::MakeSlice` (`src/kirito/bytecode_vm.hpp:322-327`): `alloc()` of the `SliceVal` runs while
  start/stop/step are still on the operand stack (peek, not pop) → rooted through any GC during alloc.
- `SliceVal::getAttr` `.indices()` (`src/kirito/runtime.hpp:427-451`): the three fresh `makeInt`s are added
  to a `RootScope` before the `ListVal` is built and before `alloc()` of the list → survive GC (correct
  non-barriered-build pattern).
- `InstanceValue::slice` (`src/kirito/runtime.hpp:2044-2049`): the fresh `SliceVal` is `RootScope`-rooted
  across the user `_getitem_` call, keeping its bounds live via `children()`.
- Ellipsis singleton is a GC root (`src/kirito/vm.hpp:40,166`), so `...` survives collection.
- Soak tests, all clean under asan + `KIRITO_GC_THRESHOLD=1` with fresh (non-interned) big-int bounds and
  garbage churn between build and use:
  - `slice(big,big,big)` build + `.start/.stop/.step` read + `.indices(big)` in a 200-iter loop → correct
    arithmetic, no asan/UBSan report.
  - Multi-axis `c[b1:b2:7, k]` (MakeSlice + user `_getitem_`) in a 300-iter loop → correct, clean.
  - Single-axis `xs[lo:hi:2]` / `str[lo:hi]` (GetSlice) in a 300-iter loop → correct, clean.

## `.indices(length)` correctness — matches CPython exactly (CONFIRMED)

Verified `slice(...).indices(n)` against `python3` `slice.indices` for: full/empty, positive step,
negative step, `None` bounds, negative start/stop, out-of-bounds (`-100`/`100`), zero-length range,
`slice(-1,-100,-1)`. All 10+ cases identical to Python. Failure paths: `step == 0` →
`slice step cannot be zero`; negative length → `slice.indices: length must be non-negative`
(`src/kirito/runtime.hpp:431,434`). No signed-overflow UB in the `x += n` wrap: it runs only when `x < 0`,
so `x + n` is bounded within int64 (probed with INT64_MIN+1 bounds and INT64_MAX length under UBSan —
clean).

## Backward-compat single-axis — unchanged, matches Python (CONFIRMED)

`xs[:]`, `xs[::]`, `xs[::-1]`, `xs[2:8:3]`, `xs[-3:]`, `xs[:-3]`, `xs[-100:100]`, `xs[8:2:-2]`, `xs[100:]`,
`"...."[::-2]`, empty `s[3:3]` — all identical to Python and routed through `GetSlice`
(`src/kirito/compiler.hpp:696-704`, `src/kirito/bytecode_vm.hpp:316-320`). `resolveSlice`
(`src/kirito/stdlib_tensor.hpp:775-794`) is the SSOT clamp used by list/string/tensor.

## Parser — all edge cases behave (CONFIRMED)

- `x[]` → parse error "expected an index expression inside '[ ]'" (`parser.hpp:649`).
- `x[a:b:c:d]` (4 colons) → parse error "expected ']'" (only 2 colons consumed, `parser.hpp:659-668`).
- Slice literal outside a subscript (`var s = 1:2`) → parse error "expected end of statement".
- Chained `x[1:2][0]` → works (`20`).
- `x[None]` / `x[...]` on a List → clean runtime error ("index must be Integer, not 'None'/'Ellipsis'").
- Multi-axis on List/String/Dict → clean errors ("takes exactly one index" / "does not support slicing"),
  no crash.
- `x[a:b:c, d:e]`, `x[i, a:b]`, `x[:, :, :]`, `x[..., 0]` on a user class → all deliver correct
  `Slice`/`Integer`/`Ellipsis` args with correct `.start/.stop/.step`, `type()=="Slice"`.
- Tensor `t[0:2, 1:3]`, `t[..., 0]`, `t[None, :, :]`, two-ellipsis rejection ("at most one ellipsis") →
  correct.

## Ellipsis as a value — correct (CONFIRMED)

`type(...)=="Ellipsis"`, `String(...)=="..."`, `... == ...` → True, `... == 5` → False, truthy → True,
hashable (usable as a dict key). `foldConstValue` (`src/kirito/compiler.hpp:187-219`) returns `nullopt`
for `EllipsisTag` (the `LiteralExpr` branch has no matching alternative), so `...` is **not** const-folded:
`... + 1` errors at runtime ("type 'Ellipsis' does not support this binary operator") and a `case ...:`
switch label is rejected ("must be a constant scalar"). Confirmed not folded into arithmetic/switch paths.

## Hashability & serde — correct, fail-loud (CONFIRMED)

- `Slice` is unhashable (default `Object::hashable()==false`, `object.hpp:179`): dict-key / set use →
  clean "unhashable type 'Slice'". Matches Python.
- `Ellipsis` is hashable (`builtins.hpp:169-170`): usable as a dict key.
- `serialize`/`dump` of a `Slice` or `Ellipsis` → clean "cannot serialize type 'Slice'/'Ellipsis'"
  (explicit failure, no silent fallback).

## Verified CORRECT (summary checklist)

- SliceVal::children() enumerates all three bounds; GC-safe during MakeSlice, .indices(), InstanceValue::slice, slice() builtin.
- Ellipsis singleton rooted; survives GC.
- .indices() == CPython slice.indices across negative/None/step<0/OOB/zero-length; step 0 and negative length throw.
- No signed-overflow UB in bound wrapping (asan/UBSan clean).
- Single-axis list/string slicing unchanged and == Python; uses GetSlice.
- Parser: empty subscript, 4-colon, stray slice-literal, chained, None/Ellipsis on list, multi-axis on
  non-instance — all clean errors / correct results.
- Ellipsis value semantics (type/str/eq/truthy/hashable) correct; not const-folded.
- User-class single- and multi-axis `_getitem_`/`_setitem_` with Slice keys correct.
- Tensor basic indexing (slice/ellipsis-fill/newaxis, >1 ellipsis rejected) correct.
- Slice/Ellipsis unserializable and Slice unhashable — fail loudly.

## Not probed

- tsan / parallel dispatcher interaction with slicing: not exercised. SliceVal is immutable and holds no
  thread-shared mutable state, and slicing is not part of the `parallel` data-race surface, so this was
  judged out of scope; noted for honesty.
