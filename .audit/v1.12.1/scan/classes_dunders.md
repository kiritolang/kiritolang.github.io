# v1.12.1 (audit loop) — user classes, dunders, operator overloading, inheritance, _super_, private members

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

### F2 [HIGH — security] Privacy bypass: a free function adopted as a method reads private members when called directly from external scope
- where: same root cause as F1 (ownerClass stashed on KiFunction: bytecode_vm.hpp:186-192) + checkPrivateAccess reading the running frame's `ownerClass()` (runtime.hpp:2151-2164).
- repro:
```
var readpriv = Function(obj): return obj._secret     # plain module-level function
class A:
    var _init_ = Function(self): self._secret = "TOP-SECRET"
    var use = readpriv                                # ...also adopted as a method of A
var a = A()
readpriv(a)   # => "TOP-SECRET"  — external code reads A's private member
```
- actual: because `readpriv` was adopted as a method of A, its KiFunction has `hasOwner=true, ownerClass=A`. Calling it directly from module scope still runs its body with frame ownerClass=A, so `checkPrivateAccess` sees currentClass=A, receiver is an A instance -> access ALLOWED. Fully external code reads/writes `_secret`.
- expected: privacy should be bypassable only from inside code lexically belonging to the class chain, not from any external call site that happens to invoke a function object which was also registered as a method.
- fix idea: same as F1 — ownership/privacy scope must be tied to the syntactic method definition site, not carried on a first-class function value that external code can also hold and invoke.

### F3 [MED] Asymmetric `!=` when left is a dunder-less instance and right defines only `_ne_`
- where: src/kirito/runtime.hpp:2209 (reflected-right branch guarded by `l.kind() != ValueKind::Instance`) + kiEquals viaEq only inspects `_eq_`, never `_ne_` (runtime.hpp:1594-1602).
- repro:
```
class Plain:
    var _init_ = Function(self): self.x = 1
class NeOnly:
    var _init_ = Function(self): self.x = 1
    var _ne_ = Function(self, o): return False
var a = Plain()
var b = NeOnly()
a != b   # => True   (b._ne_ ignored)
b != a   # => False  (b._ne_ honored)
```
- actual: `a != b` returns True (identity fallback), `b != a` returns False (uses b._ne_). Docs assert "==/!= are symmetric".
- expected: `a != b == b != a`. When the left is an instance lacking the relevant dunder, the right instance's standalone `_ne_` should still be consulted (as it is when the left is a non-instance).
- fix idea: in applyBinaryOp let the reflected-right branch also fire when the left IS an instance but lacks a usable `_eq_`/`_ne_`; and/or have kiEquals's viaEq fall back to a standalone `_ne_` (negated). Narrow case (right defines `_ne_` but not `_eq_`), but a documented invariant is violated.
