# Coverage findings — List / Set / Dict + dunder protocol

Scope: `docs/pages/09-types.md` (List/Set/Dict methods, operators, dunder
protocol). Test: `tests/scripts/cov_containers_dunders.ki` (235 assertions,
verified against `build-release/ki`, deterministic, clean under
`KIRITO_GC_THRESHOLD=1`). No `src/` or `docs/` changes were made.

Legend: **BUG** = incorrect behavior; **DELTA** = impl differs from / is not
covered by the docs; **NOTE** = confirmed but subtle behavior worth recording.

---

## BUG-1 — native error raised inside a user `_next_` is corrupted to "dangling handle"

When a **native** exception (e.g. list index-out-of-range, dict key-not-found)
is raised inside a user class's `_next_` while it is being pulled by a `for`
loop / `List(...)` / `sum(...)`, the propagated exception loses its real message
and traceback and is replaced by an unrelated diagnostic:

    error: dangling handle (stale generation)

A **user-thrown** exception from the same `_next_` (`throw "custom error"`, or
`throw StopIteration()`) propagates correctly with the right message and
traceback. Only natively-raised errors are affected.

Minimal repro:

    class L:
        var _iter_ = Function(self):
            return self
        var _next_ = Function(self):
            return [1, 2, 3][10]        # index out of range
    for x in L():
        discard x

    # actual:   error: dangling handle (stale generation)
    # expected: error: index out of range   (with a _next_ frame in the traceback)

Contrast (works correctly):

    var f = Function():
        return [1, 2, 3][10]
    f()            # -> "index out of range", with an `f` frame  (CORRECT)

- Deterministic (reproduces every run, and under `KIRITO_GC_THRESHOLD=1`).
- Reproduces via `for`, `List(gen)`, and inside a `try/catch` (the caught value
  is the wrong "dangling handle" error).
- Also reproduces with a named local (`var y = [1,2,3]; return y[10]`) and with
  a dict key error (`{"a":1}["z"]`), so it is not specific to a temporary
  literal — it is about a native error object crossing the `_next_`/iteration
  boundary.
- Docs (09-types.md, "Lazy generators") promise that an error leaking from a
  deeper call inside `_next_` "surfaces as an error, so a bug can't masquerade
  as 'iteration finished'." It does still surface as *an* error (iteration is
  not silently ended), but the **what/where** contract from CLAUDE.md
  ("Structured, diagnostic errors … what + where + context") is violated: the
  true cause is discarded. Likely a GC/handle-lifetime issue in the
  iterator-exception unwinding path (the KiritoError's referenced handle is
  invalidated before it is reported).

Test stance: the test only asserts that this case **throws** (`threw(...)`), so
the buggy message is not locked in. Fixing the bug will not break the test.

---

## DELTA-1 — `_getattr_` and `_repr_` are not part of the protocol

The task brief listed `_getattr_` and `_repr_?` as candidate dunders. Neither is
implemented, and neither appears in the docs' special-methods tables. Grepping
`src/kirito/*.hpp` for `"_..._"` string literals yields exactly this dunder set:

    _add_ _bool_ _call_ _contains_ _div_ _enter_ _eq_ _exit_ _floordiv_ _ge_
    _getitem_ _getstate_ _gt_ _hash_ _init_ _iter_ _le_ _len_ _lt_ _mod_ _mul_
    _ne_ _neg_ _next_ _not_ _pow_ _setitem_ _setstate_ _str_ _sub_ _super_

Defining `_getattr_` on a class does **not** intercept attribute access:
`Proxy().foo` throws `'Proxy' object has no attribute 'foo'`. There is also no
`_setattr_`. This matches the docs (which never claim these exist); recorded only
because the brief assumed them. No test asserts them.

---

## DELTA-2 — `Set.pop()` is LIFO / deterministic, docs say "arbitrary"

Docs: "`s.pop()` — Remove and return an **arbitrary** element." Actual behavior
is deterministic **LIFO** (removes the last-inserted element): `{10,20,30}.pop()`
returns `30`, then `20`. This is stronger than documented (a stable, insertion-
ordered Set popping from the end), so scripts relying on the doc's "arbitrary"
wording still hold. The test asserts the actual LIFO order.

---

## DELTA-3 — empty `Set` and empty `Dict` stringify identically as `{}`

`String(Set()) == "{}"` and `String({}) == "{}"`. The two empty containers are
indistinguishable in their text form (they are distinguishable by `type(...)`:
`"Set"` vs `"Dict"`, and by the fact that `{}` constructs a Dict while `Set()`
constructs a Set). The 09-types.md printing note only calls out the empty-string
element case (`['']` vs `[]`), not this one. Minor/cosmetic.

---

## NOTE-1 — cross-type equal keys: `1` and `1.0` merge; `True` is distinct

Confirmed for both Set and Dict, matching the Float doc ("`1.0` and `1` share one
key") and the Bool doc ("`Bool` is a distinct type … `True != 1`"):

- `Set()` with `1`, `1.0`, `True` added -> length **2** (`{1, True}`); `1 in s`,
  `1.0 in s`, and `True in s` are all True (1/1.0 are one element).
- `Dict` `d[1]=1; d[1.0]=2; d[True]=3` -> length **2**, `d[1] == 2` (the `1.0`
  assignment overwrote key `1`'s value; the stored key stays `1`), `d[True] == 3`.

## NOTE-2 — NaN as a Set/Dict key is write-only

`Set().add(math.nan)` succeeds (`len == 1`) but `math.nan in s` is `False`
(NaN != NaN). Each subsequent `add(math.nan)` is treated as a **new** element
(never equal to any existing), so N inserts give length N. Consistent with the
Float equality rules; effectively unrecoverable once stored.

## NOTE-3 — mutation during iteration

- **List**: `for` snapshots the starting length — appending inside the loop does
  not extend the iteration (`for v in [1,2,3]` where the body appends still sees
  exactly `1,2,3`).
- **Dict / Set**: iterating does not raise even if the body mutates the
  container (no "changed size during iteration" guard). Not asserted beyond
  "does not throw" to keep the test deterministic.

## NOTE-4 — confirmed error-message substrings (used as hardening assertions)

List: `pop from empty List`, `pop index out of range`, `remove: value not in
List`, `index: value not in List`, `index out of range`, `can only repeat List
by an Integer`, `can only concatenate List to List`, `cannot order 'String' and
'Integer'`.
Set: `pop from an empty Set`, `remove: value not in Set`, `unhashable type
'...'`, `does not support this binary operator` (operator forms reject non-Set
rhs).
Dict: `pop: key not found`, `popitem: dictionary is empty`, `key not found: <k>`
(both `remove` and `[]`), `... is not iterable` (update from non-iterable).
Dunder: `unsupported operand type ...` (reflected op), `'<C>' has no operator
'_div_'` / `'_lt_'`, `'<C>'._bool_ must return a Bool`, `'<C>'._hash_ must
return an Integer`, `_len_ must return an Integer` / `... a non-negative
Integer`, `unhashable type '<class>'` (no `_hash_`), `'<C>' object is not
callable`, `type '<C>' is not iterable`, `'<C>' object has no attribute
'_enter_'`, `function missing required argument '<name>'`. Default instance str
is `<Class object>`.
