# A09 — classes, dunders, inheritance, privacy, `_super_` (round v1.15.1)

Scope: `src/kirito/class_value.hpp` + class/dunder dispatch in `runtime.hpp` / `bytecode_vm.hpp`.
Probing binary: `./build-debug/ki` (no builds performed — per round rule).

Status: IN PROGRESS

## Findings

### A09-1: a nested function inside a method loses class ownership — cannot touch `self._private`  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/bytecode_vm.hpp:234-248` (ownership stamped only on the class-body method
  clone), `src/kirito/function.hpp:134` (`KiFunction::ownerClass` / `hasOwner`),
  `src/kirito/runtime.hpp:2184` (`checkPrivateAccess`).
- What: `ownerClass` is attached to the (class, method) binding at **class-build time**. A `Function`
  literal created **at runtime inside a method body** is a fresh `KiFunction` with `hasOwner = false`,
  so `checkPrivateAccess` sees no current class and denies. CLAUDE.md's contract is that a private
  member is "accessible only from within a method of the same class **or a subclass**" — code lexically
  inside a method *is* within that method. Today, the moment you need a helper closure / a callback that
  reads private state, privacy breaks. Note the failure direction is **deny**, not allow — so this is a
  usability/correctness gap, not a privacy hole.
- Repro:
```ki
class Bag:
    var _init_ = Function(self):
        self._items = [500, 300, 900]
    var total = Function(self):
        var acc = 0
        var add = Function(v):
            acc = acc + v + self._items[0] * 0
        for v in self._items:
            add(v)
        return acc
    var direct = Function(self): return self._items
var b = Bag()
io.print(String(b.direct()))
io.print(String(b.total()))
```
```
$ ./build-debug/ki /tmp/a09_p5.ki
direct:      [500, 300, 900]
total ERR: cannot access private member '_items' of 'Bag' outside its class
```
  Note `sorted(self._items, key = Function(x): return -x)` *works* — because `self._items` is read in
  the method's own frame; only a private touched **inside** the nested body fails.
- Impact: anyone writing a closure/callback over private state inside a method. Silent-ish (a clear
  error), but it forces either dropping the underscore (losing privacy for the whole class) or hoisting
  every private read to the method frame.
- Proposed fix: propagate the enclosing frame's owner into a `KiFunction` created by a `MakeFunction`
  op executed in a frame with `hasOwner_` (i.e. inherit `ownerClass`/`hasOwner` from the defining
  frame when the function has none). This is additive — it only ever *grants* access that the
  contract already promises, and cannot make a currently-allowed access fail. Care: the class-build
  clone comment (bytecode_vm.hpp:225-233) warns ownership must never be stamped on a *shared* function
  object; a nested literal is freshly allocated per execution, so inheriting at creation is safe.
- Proposed test: `tools/tests/scripts/` — a `.ki` asserting a closure inside a method reads/writes
  `self._priv`, plus a C++ `test_classes.cpp` case. Must FAIL on the unfixed build.

### A09-2: `self._super_()` is unavailable inside a nested function in a method  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/runtime.hpp:2310` (`evalMemberGet` only synthesizes `_super_` when
  `hasCurrentClass`), same root cause as A09-1.
- What: `_super_` is synthesized by `evalMemberGet` only when the executing frame has an owner class.
  Inside a nested function the owner is gone, so `self._super_()` does not resolve at all and falls
  through to the ordinary attribute lookup, producing a **misleading** error: `'Sub' object has no
  attribute '_super_'` — as if `_super_` did not exist, rather than "not inside a method".
- Repro:
```ki
class Base:
    var greet = Function(self): return "base-greet"
class Sub(Base):
    var lam_super = Function(self):
        var f = Function(): return self._super_().greet()
        return f()
io.print(String(Sub().lam_super()))
```
```
$ ./build-debug/ki /tmp/a09_p6.ki
lambda calls self._super_() -> ERR: 'Sub' object has no attribute '_super_'
```
- Impact: any deferred/callback use of an inherited implementation. The error text actively misleads.
- Proposed fix: same owner-inheritance as A09-1 (fixes both at once). If owner inheritance is
  rejected, at minimum the error should say `_super_() is only available inside a method`.
- Proposed test: `.ki` asserting `self._super_().greet()` works from a closure inside a method.

### A09-3: method/constructor arity errors count the implicit `self` — the numbers match nothing the user typed  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/runtime.hpp:1698-1707` (`ClassValue::callFull` prepends `instH` to `full`),
  `src/kirito/runtime.hpp:1717-1732` (`makeBoundMethod` prepends `receiver`); the arity message is
  produced downstream by `KiFunction::callFull`, which only sees the already-widened span.
- What: the receiver is spliced in before argument binding, so the diagnostic reports the *internal*
  arity. CLAUDE.md: "Clear diagnostics: lexer/parser/runtime errors should carry line and column and a
  message a user can act on. Errors are part of the language, not an afterthought." Here neither number
  is actionable — `p.meth(1, 2)` on `meth = Function(self, a)` reports "takes 2 ... but 3 were given",
  while the user wrote one parameter's worth of call site and passed two. The message also says
  "function" rather than naming the method/class.
- Repro:
```ki
class Pt:
    var _init_ = Function(self, x, y = 900):
        self.x = x
        self.y = y
    var meth = Function(self, a): return a
var p = Pt(400)
p.meth(1, 2)
```
```
$ ./build-debug/ki /tmp/a09_p11.ki
ctor extra positional: user passed 3 -> ERR: function takes 3 positional argument(s) but 4 were given
method extra positional: user passed 2 -> ERR: function takes 2 positional argument(s) but 3 were given
plain fn extra positional (baseline) -> ERR: function takes 1 positional argument(s) but 2 were given   # correct
```
  (`Pt(1,2,3)` → "takes 3 … but 4 were given": the user passed 3 to a 2-parameter constructor.)
- Impact: every user who miscounts a method's arguments. The baseline plain-function message is
  correct, so the defect is specific to the receiver-prepending call paths.
- Proposed fix: pass a "hidden leading arg" count (1 for bound-method / instantiation paths) down to
  the arity check so it subtracts `self` from both numbers, and name the callee. Does not touch any
  documented contract — the *binding* is unchanged, only the message.
- Proposed test: `tools/tests/errors/` — a `.ki` + `.experr` pinning the method arity text; plus a
  `.experr` for the constructor form.

### A09-4: `_setstate_` without `_getstate_` is silently never called (the mirror case is a hard error)  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/stdlib_serde.hpp:222` (flatten only probes `_getstate_`) vs `:636-638`
  (rebuild throws when `_getstate_` was used but `_setstate_` is missing).
- What: the protocol is validated in exactly one direction. A class defining only `_getstate_` fails
  loudly at load — `cannot deserialize 'GetOnly': it defines _getstate_ but no _setstate_`. A class
  defining only `_setstate_` takes the attribute-based path and the user's `_setstate_` is **never
  invoked**, with no diagnostic. A `_setstate_` typically exists to rebuild a cache / re-derive
  non-serialized state; here it is dead code the author believes runs.
- Repro:
```ki
class SetOnly:
    var _init_ = Function(self, v):
        self.v = v
        self.trace = "init"
    var _setstate_ = Function(self, st): self.trace = "setstate-called-with:" + String(st)
var s = dump.loads(dump.dumps(SetOnly(3)))
io.print("SetOnly: v=" + String(s.v) + " trace=" + String(s.trace))
```
```
$ ./build-debug/ki /tmp/a09_p14.ki
SetOnly: v=3 trace=init          # _setstate_ never ran; no error, no warning
```
- Impact: a class that relies on `_setstate_` to rebuild derived state silently deserializes into a
  half-initialized object. Low frequency (the pairing is documented), but silent.
- Proposed fix: symmetric validation — when a class defines `_setstate_` but no `_getstate_`, throw at
  **flatten** time (`cannot serialize 'X': it defines _setstate_ but no _getstate_`), mirroring the
  existing message. Alternative (weaker): an analyzer warning. Does not affect classes that define
  both, or neither.
- Proposed test: `tools/tests/scripts/` serde spec — assert the new error text; and pin that a class
  with **both** still round-trips (the existing behaviour).

### Non-finding (verified by reading + probe): `_getstate_` may return ANY value, not just a Dict
`_getstate_` returning `12345` round-trips and is handed verbatim to `_setstate_` (`got=12345`). The
state is an opaque payload; this is flexibility, not a defect. Recorded so the next round doesn't
re-flag it.

## Log
