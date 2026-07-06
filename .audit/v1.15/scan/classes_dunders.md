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

### F1 [HIGH] Aliased method function shared across classes gets its `ownerClass` overwritten (corrupts `_super_` + private access)
- where: src/kirito/bytecode_vm.hpp:186-192 (BuildClass unconditionally sets `fn.ownerClass = clsHandle; fn.hasOwner = true` on every method KiFunction). `ownerClass` is a single mutable field on `KiFunction` (function.hpp:134-135), but a KiFunction can be referenced as a method by more than one class.
- repro (`_super_` corruption):
```
class Base1:
    var who = Function(self): return "Base1"
class Base2:
    var who = Function(self): return "Base2"
var callsuper = Function(self): return self._super_().who()
class D1(Base1):
    var m = callsuper
class D2(Base2):
    var m = callsuper
var d1 = D1()
d1.m()   # => "Base2"  (WRONG, expected "Base1")
```
- repro (private false-DENY):
```
var readpriv = Function(self): return self._secret
class A:
    var _init_ = Function(self): self._secret = "A-secret"
    var reveal = readpriv
class B:
    var _init_ = Function(self): self._secret = "B-secret"
    var reveal = readpriv
A().reveal()   # THROW: cannot access private member '_secret' of 'A' outside its class  (WRONG)
B().reveal()   # "B-secret" (works — B was the last class defined)
```
- actual: the LAST class to reference the shared function wins; every earlier class's method now has the wrong `ownerClass`, so `self._super_()` climbs from the wrong base and `checkPrivateAccess` uses the wrong `currentClass` (legit access wrongly denied; and in inheritance-related cases could mis-permit).
- expected: each class's method should resolve `_super_`/privacy against the class it is a member of, regardless of function-object identity.
- fix idea: `ownerClass` cannot live on the shared function. Either (a) detect an already-owned KiFunction and clone it when it is adopted as a method by a second class, or (b) resolve the owning class from the call site (the class through whose `methods` map the bound method was found) rather than from a field stashed on the function. Note: normal inline `var m = Function(self): ...` bodies are unaffected because each is a distinct object; only a function object shared between classes triggers it.
