# v1.12.1 (audit loop) — object model, arena, GC, pool, Value/handle API

Subsystem source: object.hpp, arena.hpp, pool.hpp, value.hpp, native.hpp, vm.hpp, module.hpp, function.hpp

## FINDINGS

### F1 [MED] value.hpp comparison operators do an unchecked `static_cast<const BoolVal&>` on a dunder result — UB when `_eq_`/`_lt_`/… returns a non-Bool
- where: src/kirito/value.hpp:1021-1044 (`Value::operator==`, `!=`, `<`, `<=`, `>`, `>=`)
- code:
  ```cpp
  inline bool Value::operator==(const Value& r) const {
      Handle h = applyBinaryOp(*vm_, BinOp::Eq, h_, r.h_);
      return static_cast<const BoolVal&>(vm_->arena().deref(h)).value();   // <-- unchecked
  }
  ```
- root cause: for a user `class` that defines `_eq_`/`_ne_`/`_lt_`/`_le_`/`_gt_`/`_ge_`,
  `applyBinaryOp` returns the **raw** dunder result and does NOT coerce it to a Bool
  (runtime.hpp:2197-2198 for Eq/Ne → `l.binary(...)`; ordering ops dispatch through
  `InstanceValue::binary`→`invokeOp` which returns the raw method value, runtime.hpp:1845-1848).
  Kirito deliberately permits this (documented "returns the raw value" behaviour, same family as
  `_not_`/`_neg_`). So `applyBinaryOp(BinOp::Eq, ...)` can legitimately return an IntVal / StrVal /
  anything. The C++ ergonomic operators then `static_cast<const BoolVal&>` that object and read
  `.value()` — a type-confused cast (UB): reads a bool off the wrong object layout, returns garbage,
  and is not caught by the usual peek-before-wrap discipline every other accessor in value.hpp uses.
- repro (confirms the raw-return that feeds the bad cast; Kirito level, no coercion):
  ```
  class Weird:
      var _eq_ = Function(self, other): return 42
      var _lt_ = Function(self, other): return "less"
  var a = Weird()
  var b = Weird()
  io.print(a == b, type(a == b))   # => 42 Integer   (NOT a Bool)
  io.print(a < b,  type(a < b))    # => less String
  ```
  A host embedding Kirito that then evaluates `Value(a) == Value(b)` in C++ executes
  `static_cast<const BoolVal&>(<IntVal 42>).value()` → UB / type confusion.
- actual: unchecked `static_cast<const BoolVal&>` on a non-BoolVal Object (embedding-only, C++ path).
  expected: consume the result with truthiness, like every other dunder-result consumer in runtime.hpp
  (e.g. `InstanceValue::contains` line 1893, `kiEquals` line 1598 both use `.truthy()`).
- fix idea: replace the six `static_cast<const BoolVal&>(vm_->arena().deref(h)).value()` with
  `vm_->arena().deref(h).truthy()`. Byte-identical for a genuine Bool, safe for a raw non-Bool.
- severity note: embedding-only (no Kirito-level UB — `if a==b:` goes through `truthy()`), and needs
  a user dunder that returns a non-Bool, which the language explicitly allows. Trivial, safe fix; the
  public C++ operator surface should never UB on a value the language itself produces.

## LOG — examined and ruled out
- **arena.hpp** — slot+generation model solid. Gen 0 reserved as `Handle{}` sentinel; real gens
  [1,UINT32_MAX]. `alloc` reuse from free-list does not re-bump generation (already bumped at sweep) —
  correct. Generation-wraparound ABA handled by permanently RETIRING a slot at UINT32_MAX (occupied=
  false, off free-list) so a stale `{slot,UINT32_MAX}` handle can never re-validate — correct. `at()`
  checks range + occupied + generation → clean "dangling handle" throw. `markIfUnmarked` checks
  occupied+generation+marked. Leftover `marked` on a reused slot is harmless (clearMarks runs first
  every cycle, and markIfUnmarked is only called inside collectGarbage). No bug.
- **pool.hpp** — segregated free-list, kAlign=16, kMaxPooled=224, classOf math checked at 1/16/224/225
  boundaries — exact. Block reuse always safe (block size ≥ any n mapping to its class). Sanitizer
  bypass present. Over-aligned types re-routed to global aligned new via Object::operator
  new(align_val_t) (object.hpp:92). thread_local FreeLists dtor drains on thread exit (fixes the
  per-spawn leak). No bug.
- **object.hpp** — protocol base; children() default empty. Over-aligned allocation guard present.
  No bug.
- **GC rooting (collectGarbage, vm.hpp:127-158)** — cross-checked every VM-held handle region is a
  root: none_/true_/false_/undefined_/global_, smallInts_, replScope_ (guarded), moduleCache_,
  pathCache_, arglist_ (guarded), classRegistry_, tempRoots_, pinnedRoots_, bytecodeConsts_, auxRoots_
  (live operand stacks). Nothing missing. importing_/importStack_ are string sets (not handles);
  module-in-progress is rooted via the eval operand stack.
- **children() completeness — traced EVERY Object subclass** against its Handle members:
  - function.hpp: NativeFunction → captures_ + per-param defaults ✓; KiFunction → closure_ + ownerClass
    (guarded by hasOwner) ✓.
  - module.hpp: ModuleValue → all members ✓.
  - class_value.hpp: ClassValue → methods + base(guarded); InstanceValue → cls + attrs; SuperValue →
    instance + startClass. selfHandle is self-referential (correctly omitted). ✓
  - environment.hpp: EnvValue → vars_ + parent_(guarded) ✓.
  - collections.hpp: ListVal → elems; DictVal → keys+values via children; SetVal → items ✓.
  - bytecode_vm.hpp: IterCursor → items + source(when lazy) ✓.
  - stdlib_net.hpp: ResponseVal/SessionVal → headersH+cookiesH (body is std::string, no handle) ✓.
  - stdlib_regex.hpp: MatchVal → subject ✓; RegexVal holds no handles ✓.
  - stdlib_tensor.hpp: TensorVal → node->parents; autograd backward closures capture COPIES of tensor
    DATA (acopy/bcopy as FT), never handles, so no rooting gap ✓.
  - stdlib_io.hpp (FileVal/BytesIO/StdStream), stdlib_random (RandomState), stdlib_time (DateTime),
    stdlib_matrix/complex, stdlib_parallel primitives: hold NO live handles (OS resources / numeric
    state / serialized blobs) — no children() needed ✓.
  → No GC-rooting gap found anywhere.
- **value.hpp pinning** — RAII `detail::Pin` (shared_ptr, pin once/unpin once, copy-shared);
  `adopt()` pins every fresh-alloc wrapper; `adopting()` used on every fresh-handle return
  (operators, call, getAttr, pop, at, items). `items()` pins each element (String/Bytes yield fresh
  boxes). PinnedHandle copy/move/assign all refcount-correct and self-assignment-safe. No dangling-
  handle-return found besides the note that Dict/Set keys()/values()/pairs() return unpinned views —
  by design (elements rooted by the still-live container). No bug.
- **NativeFunction::bindArgs** (function.hpp:78) — positional fill → keyword-by-name → defaults;
  too-many-positional / unknown / duplicate / missing all raise clean errors. PROBED via `pow`,
  `round` (kw, dup, unknown, too-many-positional) — all correct.
- **makeMethod** (native.hpp:176) — keyword binding, None-fill of optional middle holes, minArgs
  guard for required leading slots. PROBED: `d.get(default=99)`→missing key; `insert(item=5)`→missing
  index; `replace(...,count=2)`, `split(sep,maxsplit)`, `find(...,start)`, `center(width,fillchar)` —
  all correct. Bound-method captures pass `{self}` as GC captures at every call site checked.
- **sliceIndices** (native.hpp:54) — PROBED full/step/neg-step/oob/zero-step/reverse/unicode and
  INT64_MIN/INT64_MAX step & start — no overflow, no crash, correct results. Count-driven loop avoids
  the `i+=step` overflow. Solid.
- **requireNoNulPath** — PROBED: embedded-NUL paths rejected in path.exists / path.getsize / io.open.
- **Args** (value.hpp:847) — `operator[]` unchecked by design; `at`/`require` bounds-checked with
  uniform wording. No bug.
- **EnvValue SmallVec** — inline-4 small-buffer, grow doubling from cap_=kInline(4), move-constructs
  on grow, destroys elements. cap_ never 0. No bug.
