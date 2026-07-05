# Discrepancies — operators & special methods (dunders) on user classes
# (docs/pages/09-types.md "Special methods / operator overloading" + 02-language-guide.md
#  vs src/kirito/{runtime,class_value}.hpp)

Coverage: verify_operators.ki — 116 `assert` lines. Every documented dunder is exercised for
functioning (operator/construct dispatches to it) and hardening (wrong return type / missing dunder /
cross-type misuse raises the RIGHT message): `_init_`, `_str_`, `_add_/_sub_/_mul_/_div_/_floordiv_/
_mod_/_pow_`, `_eq_/_ne_/_lt_/_le_/_gt_/_ge_`, `_neg_/_not_`, `_bool_`, `_call_`, `_getitem_/
_setitem_` (scalar + variadic `m[i,j]`), `_len_`, `_contains_`, `_iter_`, `_enter_/_exit_`, `_hash_`.
Plus privacy (`_member`), `self._super_()` (extends inherited method, climbs one level, throws with no
base), shared class `var`s (copy-on-write), reflected `==`/`!=`, cross-type operator hardening, and
exact IEEE-754 float `==`.

**Overall the docs match the implementation.** Every documented error message and dispatch rule was
confirmed byte-for-byte. Two minor silent/undocumented behaviours are pinned by the test:

### operators-1: a non-String `_str_` return is silently stringified, not rejected
- kind: doc-vs-impl (silent — a misuse that arguably should raise but doesn't)
- location: docs/pages/09-types.md:317 (the `_str_` row says **Returns → "a String"**) ;
  src/kirito/runtime.hpp:1907-1924 (`InstanceValue::str`)
- expected (per docs): `_str_(self)` returns a `String`. By analogy with `_bool_` ("must return a
  Bool" throws) and `_hash_` ("must return an Integer" throws) and `_len_`, a wrong return type would
  be expected to throw a clear message.
- actual: `InstanceValue::str` only special-cases a `String` return; **any other return type is
  recursively `str()`-ed** and used as the display text with no error. So a `_str_` that
  `return 42` makes `String(inst) == "42"`. No throw, no diagnostic.
- resolution: test PINS current behaviour
  (`assert String(StrInt()) == "42", "PIN: non-String _str_ return is stringified, not rejected"`).
  Not a soundness bug (the value is still rendered), but it is an inconsistency with the sibling
  return-type-checked dunders and with the doc's stated return type. A one-line doc note, or a
  return-type check in `str()`, would close it. No `src/` change — flagged, not fixed.

### operators-2: a shared class `var` is unreadable directly on the class value
- kind: doc-vs-impl (behaviour narrower than the prose might suggest)
- location: docs/pages/09-types.md (Special methods intro) + CLAUDE.md "Non-function class-body `var`s
  are shared class attributes: instances read them through the class and copy-on-write on assignment" ;
  src/kirito/runtime.hpp `ClassValue`/`getAttr` (a `ClassValue` exposes only its *methods*, not
  class-body `var`s)
- expected (reading the prose loosely): a class-body `var count = 0` is a "shared class attribute".
  One might expect `ClassName.count` to read the shared default.
- actual: shared vars are readable/copy-on-write **only through an instance** (`inst.count`).
  Reading the value directly off the class object — `Shared.count` — throws
  `type 'Shared' has no attribute 'count'`. (The documented feature — instance reads the shared
  default, assignment gives that instance its own copy without disturbing siblings — works exactly as
  described; only the class-value read path is absent.)
- resolution: test PINS both: the instance copy-on-write semantics
  (`s1.count = 5` leaves `s2.count == 0`) AND the class-value read raising
  (`assert raises_msg(Function(): return Shared.count, "has no attribute 'count'")`). The docs never
  actually promise `Class.count`, so this is arguably just an unstated limit rather than a bug. No
  `src/` change — flagged, not fixed.

## Confirmed-correct highlights (no discrepancy)
- `_ne_` derives from `not _eq_` when only `_eq_` is defined; an explicit `_ne_` overrides it.
- Ordering operators do NOT derive from each other (only-`_lt_` → `>` throws `has no operator '_gt_'`).
- Left-operand-only dispatch: `3 + Money(5)` / `2 * Money(5)` throw
  `unsupported operand type 'Money' for arithmetic with 'Integer'` (no reflected `_radd_`); `==`/`!=`
  are the documented symmetric exception (`5 == RefEq(5)` consults the RHS `_eq_`).
- Return-type enforcement is exact: `_bool_` non-Bool → `'X'._bool_ must return a Bool, got '...'`;
  `_hash_` non-Integer → `'X'._hash_ must return an Integer, got '...'`; `_len_` non-Integer →
  `_len_ must return an Integer`, negative → `_len_ must return a non-negative Integer`.
- Missing-dunder messages: `object is not callable` / `is not indexable` /
  `does not support item assignment` / `has no length` / `does not support 'in'` /
  `is not iterable` / `has no operator '_op_'` / `has no attribute '...'`.
- `_hash_` without `_eq_` keys by identity; no `_hash_` → `unhashable type 'X'`.
- Privacy is per class *chain*: a subclass method reads a base `_private`; outside-any-method access
  throws `cannot access private member '_token' of 'P' outside its class`.
- `self._super_()` extends `_init_`, climbs exactly one level per call
  (`Leaf->Mid->Base(7)`), and a baseless `_super_()` throws
  `_super_() called in 'Lonely', which does not inherit from any class`.
- `_exit_` runs on exceptional `with` exit and cannot suppress the exception (return ignored).
- Strong typing: `"x" + 5` → `can only concatenate String to String, not 'Integer'`; `True + 1` →
  `type 'Bool' does not support this binary operator`; cross-type `==` is `False` (never throws).
- Exact IEEE-754: `0.1 + 0.2 == 0.3` is `False`, `nan != nan`, `inf == inf`, `0.0 == -0.0`.

No genuine bugs, wrong messages, or unsound silent successes found.
