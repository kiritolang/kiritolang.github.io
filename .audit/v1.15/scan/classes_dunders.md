# v1.15 audit — user classes, dunders, operator overloading, inheritance, _super_, private members

Worker notebook. Append findings as confirmed.

## LOG
- Started. Reading runtime.hpp, object.hpp, function.hpp.

## VERIFIED-CORRECT (ruled out, no bug)
- non-class base rejected ("base class must be a class").
- multi-level `_super_` chain (C+B+A) works; climbs one level per call; no-base super throws.
- private: external denied, sibling denied, subclass reads base private allowed. `_super_()._priv`
  works (legit — subclass accessing base private).
- `_bool_` non-Bool / `_len_` negative / `_len_` non-Int / `_hash_` non-Int all checked & throw.
- class-attr COW (scalar) + mutable-class-attr in-place aliasing (documented) both as specified.
- kw args to `_init_` and methods; default referencing earlier param; mutable default fresh each call.
- isinstance sub->base True, base->sub False; typed catch matches subclass as base.
- `_iter_` returning self -> depth-guard throws; `_str_` returning self -> cycle guard "<S object>".
- reflected `!=` symmetric; `==` with only `_ne_` uses identity (by design); kiEquals viaEq symmetric
  (a==b honors b's `_eq_` even when a lacks it).
- non-function dunder (`_add_=5`) -> graceful "not callable".
- variadic `_getitem_`/`_setitem_` (m[i,j]) work; single-key too.
- overriding `_super_` works (documented "shouldn't but can").

## FINDINGS
