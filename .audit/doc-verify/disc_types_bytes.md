# Discrepancies — Bytes type (docs/pages/09-types.md vs src/kirito/bytes.hpp, runtime.hpp)

## 1. `sorted()` / `min()` / `max()` cannot order Bytes, though `<`/`>` can

**Severity:** minor doc/impl mismatch (pinned in the test, suite stays green).

**Docs** (`09-types.md`, line 171): "`Bytes` is hashable (a Dict/Set key), serializable, **ordered
lexicographically**, and supports `+` (concatenate) and `*` (repeat)." Nothing distinguishes the
operators from the ordering builtins.

**Impl:** `BytesVal::binary` (bytes.hpp:131-139) implements lexicographic `<`/`<=`/`>`/`>=`, so the
operators work:

```kirito
Bytes([1, 2]) < Bytes([1, 3])   # True
```

But `sorted()`/`min()`/`max()` (and List-element ordering) go through `kiLessThan`
(`runtime.hpp:393`), whose type ladder handles Numbers, String, List/Array, and user-class `_lt_`
instances — **but has no Bytes case** — so it falls through to:

```
cannot order 'Bytes' and 'Bytes'
```

```kirito
sorted([Bytes([2]), Bytes([1])])   # throws "cannot order 'Bytes' and 'Bytes'"
min([Bytes([2]), Bytes([1])])      # throws
```

So a type documented as "ordered lexicographically" is not sortable via the standard ordering
builtins. Two consistent resolutions:
- **Fix (src/):** add a `BytesVal` branch to `kiLessThan` (`x.data < y.data` when both are Bytes),
  mirroring the String branch — makes `sorted`/`min`/`max` agree with the `<` operator.
- **Doc-only:** narrow the docs to say the ordering is via the comparison operators only.

Current behaviour (throwing) is PINNED in `verify_types_bytes.ki` so the suite stays green:
`raises_msg(... sorted([Bytes([2]), Bytes([1])]) ..., "cannot order 'Bytes' and 'Bytes'")`.

No `src/` edit made (per audit rules — flagged first).
