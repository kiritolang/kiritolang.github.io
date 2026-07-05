# A16 — matrix + complex audit

Area: `src/kirito/stdlib_matrix.hpp` (real matrices / vectors, a 2-D `tensor::Tensor<double>`),
`src/kirito/stdlib_complex.hpp` (complex numbers + complex matrices, `tensor::Tensor<cdouble>`).
Both are Kirito-facing 2-D views over the shared `tensor.hpp` engine.

Method: static reasoning only (no build/run). Cross-referenced the tensor engine
(`determinant`/`inverse`/`matmul`/`add`/`sub`/`scalarOp`/`transpose`/`trace`) and the GC/rooting
discipline in `vm.hpp` + `runtime.hpp`. Existing tests reviewed:
`tools/tests/unit/test_complex.cpp`; scripts `spec_matrix.ki`, `spec_complex.ki`, `audit_matrix.ki`,
`audit_complex.ki`, `r4_linalg.ki` (exhaustive), `r6/r7/r8_*`, `r9/r10/r11`, `spec_domain_guards.ki`.

Overall the two modules are **very** well covered — `r4_linalg.ki` alone exercises 1×1 / 0×0 / empty /
rectangular / singular / non-square / dimension-mismatch / ordering / domain-error / real-coercion /
keyword-arg / inspect surface for both modules, and the singular-inverse-throws + non-square-throws +
scalar-on-LEFT-throws cases are all asserted. DRY reuse of the tensor engine is largely honored. The
findings below are the residue: one genuine latent GC bug, one domain-guard inconsistency, and a set
of low-severity robustness/coverage/DRY gaps.

---

### A16-1: `MatrixVal::apply` passes an un-rooted argument handle across a Kirito call (latent GC bug)

- severity: medium
- location: `src/kirito/stdlib_matrix.hpp:237-247` (the `apply` method body, esp. lines 242-244)
- category: bug / GC-safety
- description: The real-matrix `apply` builds each element's callback argument with
  `std::array<Handle, 1> args{vm.makeFloat(m.data()[i])};` and then calls
  `vm.arena().deref(fn).call(vm, args)`. The freshly-allocated `makeFloat` handle is **not** added to
  any GC root set (`makeFloat`→`alloc` does not auto-root; see `vm.hpp:56-61` and the explicit warning
  at `vm.hpp:87-88` that "handles held in C++ locals across an allocation must be protected here").
  `KiFunction::callFull` allocates a new scope as its very first action
  (`runtime.hpp:1986 Handle scope = rs.add(vm.newScope(closure_));`), and any allocation inside the
  callee body can trip `collectGarbage()` (`vm.hpp:59`). At that collection point `args[0]` is
  reachable only from the C++ `std::array` on the native stack — invisible to the collector — so it can
  be swept and its slot reused, after which the callee binds a dangling/aliased handle for the
  parameter.
- failure-scenario: `matrix.Matrix(bigNxN).apply(Function(x): ... body that allocates enough to cross
  the GC threshold mid-call ...)`. In an iteration where `makeFloat` itself does not trigger GC but the
  callee body does, the parameter the callback sees is a collected float → silent wrong result or a
  crash (asan use-after-free).
- proposed-test: run `apply` over a matrix large enough (and with an allocation-heavy callback) that a
  collection is guaranteed mid-`apply`, under the `asan` preset, and assert the mapped result is exact.
  A low `gcThreshold_` would make this deterministic.
- proposed-fix: root the argument exactly as the sibling `ComplexMatrixVal::apply` already does — wrap
  the loop body in `RootScope rs(vm);` and use `rs.add(vm.makeFloat(...))` for `args[0]`
  (cf. `stdlib_complex.hpp:442-443`). This is a one-line fix bringing the real path in line with the
  complex path.
- confidence: high that the rooting is missing and inconsistent with the sibling implementation;
  medium that it is observable in practice (depends on GC timing / callback allocation volume).

---

### A16-2: `complex.atan` (and its odd cousins) not guarded at the ±i branch-point poles

- severity: low-medium
- location: `src/kirito/stdlib_complex.hpp:591` (`unary("atan", std::atan)` registered with **no** `bad`
  domain predicate), contrast with `atanh` at lines 597-601 which does guard ±1.
- category: bug / domain-guard gap
- description: The documented policy (CLAUDE.md, and the header comment at lines 565-571) is that the
  complex analytic set "throw[s] at exactly the genuine singularities — log/log10 of zero, atanh of ±1
  — while remaining defined on the rest of the plane." `atanh` correctly throws at ±1. But `atan(z)`
  has genuine poles at `z = ±i` (`atan = (1/2i)·log((1+iz)/(1-iz))`, undefined where the argument of
  log is 0). `atan` is registered with no `bad` predicate, so `complex.atan(complex.i)` returns a
  silent `inf`/`nan` instead of a `math domain error`, breaking the stated invariant. This is the exact
  atanh↔atan mirror the guard set otherwise respects.
- failure-scenario: `import("complex").atan(complex.i)` → `nan`/`inf`-valued Complex, no error. Same
  for `complex.atan(complex.of(0, -1))`.
- proposed-test: `throws(Function(): return complex.atan(complex.i))` and
  `throws(Function(): return complex.atan(complex.of(0, -1)))` — mirror of the existing
  `atanh(±1) throws` tests (`r4_linalg.ki:293-294`). Currently `atan(0)=0` is the only atan test
  (`r4_linalg.ki:276`), so this pole is entirely untested.
- proposed-fix: add a `bad` predicate to the `atan` registration that returns
  `"math domain error (atan of ±i)"` when `z == cdouble(0,1) || z == cdouble(0,-1)`, symmetric with the
  `atanh` guard. (`asin`/`acos`/`asinh`/`acosh` have no finite-plane poles and correctly need no
  guard — `r4_linalg.ki:271-275` already asserts asin(2)/acos(2)/acosh(0) do NOT throw.)
- confidence: high (clear asymmetry vs the atanh guard and the documented policy).

---

### A16-3: `MatrixVal::_setstate_` lacks the negative-dimension guard that `ComplexMatrixVal` has

- severity: low
- location: `src/kirito/stdlib_matrix.hpp:300-313` (real matrix `_setstate_`), vs
  `src/kirito/stdlib_complex.hpp:489-508` which explicitly rejects `r64 < 0 || c64 < 0`.
- category: bug / robustness (hardening of the deserialization surface)
- description: The real-matrix `_setstate_` reads `rows`/`cols` and casts straight to `std::size_t`
  without checking for a negative value (`items[0].asInt(...)` → `static_cast<std::size_t>`). A
  hand-crafted / corrupt serialized blob with a negative `cols` becomes an enormous `size_t`. Most
  paths are caught downstream (the `r > kMaxMatrixElems / c` cap when `c != 0`, or the
  `data.size() != r*c` shape check), but the corner `cols == 0` with an empty data list slips through:
  `c == 0` skips the size cap, and `r*c == 0` matches an empty `data`, so a matrix claiming
  `rows == (size_t)-1, cols == 0` is constructed. It won't immediately crash (`at()` is guarded by
  `r >= rows()`), but `rows()` then returns a garbage 2^63-scale count. The complex sibling guards this
  explicitly; the real one should too, for parity and defense-in-depth.
- failure-scenario: `dump.loads(<adversarial blob with rows=-1, cols=0, data=[]>)` yields a Matrix with
  a nonsense row count instead of a clean `"malformed state"` error.
- proposed-test: feed `_setstate_` a `[-1, 0, []]` state (or the equivalent crafted `dump` bytes) and
  assert it throws.
- proposed-fix: mirror `stdlib_complex.hpp:494-495`: read `asInt` into signed locals and throw
  `"Matrix _setstate_: dimensions must be non-negative"` before casting.
- confidence: high (the divergence from the complex path is plain; the `cols==0` escape is the only
  concretely-reachable case and is niche).

---

### A16-4: `Complex` overrides `equals` (value equality) but not `hash` → unhashable despite value equality

- severity: low
- location: `src/kirito/stdlib_complex.hpp:65-76` (`ComplexVal::equals`, incl. cross-type
  `Complex(2,0) == 2`); no `hash()` override, so it inherits `Object::hash()` which throws
  `"unhashable type 'Complex'"` (`object.hpp:93`).
- category: gap (consistency with the value model / Python parity)
- description: `Complex` has full value equality — including `Complex(2,0) == 2` — yet cannot be used
  as a `Set`/`Dict` key (any attempt throws "unhashable"). Python's `complex` **is** hashable and its
  hash agrees with the equal real. `DateTime` in `stdlib_time.hpp:173` demonstrates the intended
  pattern (override both `equals` and `hash`). `Complex` is a pure immutable value, so it is a natural
  key type; leaving it unhashable is an inconsistency with "Values are hashable where it makes sense"
  (CLAUDE.md). (Note this is *not* a correctness bug — because it is unhashable rather than
  identity-hashed, there is no eq/hash contract violation; it's a missing capability.)
- failure-scenario: `var s = {complex.i}` or `{complex.i: 1}` throws "unhashable type 'Complex'".
- proposed-test: `var d = {}` then `d[complex.of(1,2)] = 5` returns 5; and (if aligning with `==`)
  `hash(complex.of(2,0)) == hash(2)`.
- proposed-fix: add `std::size_t hash() const override` combining `std::hash<double>` of re and im,
  taking care to hash `Complex(x,0)` identically to the real `x` so the existing cross-type `equals`
  stays hash-consistent. (Matrices/ComplexMatrices are mutable and correctly left unhashable — no
  change needed there.) If Complex is intentionally kept unhashable, that intent should be documented.
- confidence: medium (whether Complex *should* be hashable is a design call; the observation is solid).

---

### A16-5: Silent overflow-to-inf in determinant / norm / matmul (no result guard)

- severity: low
- location: tensor engine `determinant` (`tensor.hpp:354-384`), `matmul` (`225-262`); the
  matrix/complex `norm` accumulators (`stdlib_matrix.hpp:282-287`, `stdlib_complex.hpp:472-477`).
- category: weak-spot (silent non-finite result)
- description: The engine rejects **input** matrices that already contain inf/NaN
  (`tensor.hpp:363,371,393,402`), but a determinant/norm/product of large **finite** elements can
  overflow the `double` product to `±inf` silently and be returned as a valid-looking result. This is
  consistent with scalar float semantics (overflow-to-inf is documented as *not* a domain error), so it
  is a weakness rather than a defect — noted for completeness. Determinant is the most exposed (product
  of `n` pivots), and `norm` squares before summing (`acc += x*x`) so it overflows at ~`sqrt(DBL_MAX)`
  element magnitude even when the true 2-norm is representable.
- failure-scenario: `matrix.Matrix([[1e200, 0], [0, 1e200]]).determinant()` → `inf` with no signal;
  `matrix.vector([1e200, 1e200]).norm()` → `inf` although the true value ~1.41e200 is representable.
- proposed-test: assert the above return `inf`/overflow and decide whether that is acceptable; if a
  hardened norm is wanted, test a hypot-style scaled norm returns the finite value.
- proposed-fix: (optional) compute `norm` with a max-scaled sum (hypot-style) to avoid premature
  overflow; determinant overflow is inherent to the algorithm and probably best left as-is + documented.
- confidence: high that the behavior is silent; low that a change is warranted (matches float policy).

---

### A16-6: DRY — `ComplexMatrix` re-implements transpose / trace / (conjugate) that the tensor engine already provides, while real `Matrix` reuses them

- severity: low
- location: `stdlib_complex.hpp:405-411` (transpose loop), `428-434` (trace loop), `412-417`
  (conjugate loop) — vs the real matrix which delegates: `stdlib_matrix.hpp:217-219`
  (`tensor::transpose`), `232-236` (`tensor::trace`). The tensor engine's `transpose`/`trace` (and the
  documented `conj` helper) are `template<class T>` and already instantiated for `cdouble`
  (determinant/inverse are reused for complex at `stdlib_complex.hpp:282-290`).
- category: DRY / maintenance divergence (not a correctness bug — the hand-rolled versions are correct)
- description: The two matrix types are described as "the same code that backs the real `matrix`,
  instantiated for `std::complex<double>`" (`stdlib_complex.hpp:199-200`), and they *do* share
  det/inverse/matmul/add/sub/scalarOp. But `ComplexMatrix` open-codes `transpose`, `trace`, and
  `conjugate` with manual index loops instead of calling `tensor::transpose` / `tensor::trace` /
  `tensor::conj`, whereas real `Matrix` calls the engine. This is an asymmetry: two code paths for the
  same operation, one of which could silently drift from the other. (`hermitian` genuinely composes
  conj + transpose and is fine to keep local, but could be `tensor::transpose(tensor::conj(...))`.)
- proposed-fix: route complex `transpose`/`trace`/`conjugate` through the tensor engine like the real
  matrix does, for single-source-of-truth. No behavior change intended.
- confidence: high (the divergence is directly visible in the two headers).

---

## Coverage gaps (no defect implied unless cross-linked to a finding above)

- **`complex.atan(±i)` untested** and unguarded — see A16-1... (A16-2). Only `atan(0)=0` is tested.
- **Real-matrix `apply` GC-safety untested** (A16-1): `apply` is functionally tested
  (`r4_linalg.ki:118-119`) but never under a mid-call collection, so the missing rooting is invisible.
- **`Matrix._setstate_` negative/adversarial-dimension path untested** (A16-3). Serialization
  round-trip is tested for *well-formed* matrices only (`audit_matrix.ki:98-103`).
- **`Complex` as a Dict/Set key untested** (A16-4) — no test asserts either hashability or the
  "unhashable" error, so the current behavior is unpinned.
- **Complex matrix `apply` GC-correctness is exercised** (`r4_linalg.ki:356`) and is correctly rooted —
  good; this is the reference implementation A16-1 should copy.
- **`complex.Matrix(rows, cols)` sized form**: unlike the real `matrix.Matrix(2,3)` (kwfn, tested at
  `r4_linalg.ki:45`), the complex `Matrix` constructor accepts only a nested list (`stdlib_complex.hpp:615`).
  This is an intentional asymmetry (use `complex.zeros`), but it is undocumented and untested — a caller
  writing `complex.Matrix(2, 2)` gets a "expects a nested list of rows"/`items()` error. Worth a doc note
  or a one-line test asserting the error, so the asymmetry is deliberate rather than accidental.
- **Overflow (A16-5)** untested for both `determinant` and `norm`.

## Non-findings verified (defenses confirmed present)

- Singular inverse throws (real + complex + zero matrix) — `tensor.hpp:403`; tested
  `r4_linalg.ki:103,359-360`.
- Non-square det/inv/trace throw — guarded at both the wrapper (`stdlib_matrix.hpp:222,227,234`;
  `stdlib_complex.hpp:283,287,430`) and the engine (`tensor.hpp:336,356,389`); tested.
- Dimension-mismatch add/sub and inner-dim-mismatch multiply throw — `stdlib_matrix.hpp:122,129`,
  `stdlib_complex.hpp:299,306`; tested.
- Scalar-on-LEFT (`2 * A`), matrix `/`, matrix `**` all throw cleanly — tested `r4_linalg.ki:85-87`,
  202-203.
- Index/dimension bounds (`m[i]`/`m[i,j]`/`get`/`set`/`row`), negative-index rejection, ragged rows,
  negative dimensions — guarded everywhere via `indexOf` + explicit `r>=rows()/c>=cols()` checks;
  tested `r4_linalg.ki:160-170`.
- Element-count cap (`kMaxMatrixElems`/`kMaxElems` = 16M) prevents `bad_alloc` on absurd dimensions —
  `stdlib_matrix.hpp:104-113`, `stdlib_complex.hpp:269-278`.
- Complex ordering (`< <= > >=`) throws "not ordered"; div-by-(complex-or-real)-zero throws; `0**0==1`;
  `0**neg`/`0**complex` throw — `stdlib_complex.hpp:120,128,106-114`; tested.
- non-finite input to det/inverse rejected up-front (avoids the misleading "singular") — `tensor.hpp:363,393`.
- EXACT `==` vs tolerant `.compare(rel_tol, abs_tol)` boundary honored for Complex / Matrix /
  ComplexMatrix (`==` bit-exact via `equals`, tolerance only in `.compare`/`is_zero`); tested.
- `-0.0` imaginary part normalized in `complexToString` (`stdlib_complex.hpp:47-48`).
- GC rooting correct in `getItem`/`row` (real + complex) and in complex `apply` (RootScope + rs.add).
- Matrix `equals`/`compare` and cross-type `Complex == real` behave per spec.
