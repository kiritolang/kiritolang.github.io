# A14 — tensor + matrix + complex + autograd (v1.15.1)

Scope: `src/kirito/tensor.hpp`, `src/kirito/stdlib_tensor.hpp`, `src/kirito/stdlib_matrix.hpp`,
`src/kirito/stdlib_complex.hpp`. Plus test coverage in `tools/tests/unit/test_tensor*.cpp`,
`test_matrix*.cpp`, `test_complex.cpp`, and `tools/tests/scripts/*tensor*.ki`, `*linalg*.ki`,
`audit_tensor.ki`.

Known false positives (NOT findings, per `.audit/README.md`):
- Left-scalar tensor arithmetic throws (`2*t` fails, `t*2` works) — documented limitation.
- `Complex` is unhashable by design.
- `complex.polar` accepts a negative finite modulus by design.

Probing with `./build-debug/ki`, every probe also under `--gc-threshold 1`.

## Findings

### A14-1: `Tensor.take()` with no arguments reads past the end of the argument span (OOB read)  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/stdlib_tensor.hpp:1966-1968` (the `take` bind)
- What: every other fixed-arity Tensor method guards with `Args(vm, a, "...").require(N)` /
  `requireArgs` before touching `a[0]`. `take` does not — it goes straight to
  `Value(vm, a[0]).items()`. `makeMethod`'s positional fast path (`native.hpp:202`:
  `if (named.empty()) return impl(v, pos);`) forwards the caller's args **verbatim, with no
  padding**, so `t.take()` dereferences `a[0]` on an **empty span**. That is exactly the UB
  `requireArgs`' own comment (`native.hpp:40-48`) says it exists to prevent:
  > "makeMethod's positional fast path forwards the call's args verbatim (no padding), so a method
  > whose body indexes a fixed `a[i]` must guard first — else it reads past the span (UB)."
- Repro: the read is a genuine OOB — the *value seen* changes with unrelated surrounding code,
  proving it reads whatever handle happens to sit past the end of the span:

```ki
var io = import("io")
var tensor = import("tensor")
var t = tensor.Tensor([[1.0, 2.0], [3.0, 4.0]])
var r = t.take()
```
```
$ ./build-debug/ki /tmp/a14_take.ki
/tmp/a14_take.ki:5:15: error: index expected Integer, got 'String'
```
Add one unrelated local (`var pad1 = [1,2,3]`) before the call and the same expression reports a
different garbage type:
```
$ ./build-debug/ki /tmp/a14_take1.ki
/tmp/a14_take1.ki:5:15: error: type 'Integer' is not iterable
```
Same under `--gc-threshold 1` (`type 'Integer' is not iterable`). A correct build must give one
deterministic arity error regardless of surrounding code.
- Impact: any caller who writes `t.take()` (or is one refactor away from it) gets a
  nondeterministic error, and the engine reads a handle it does not own. It is a span overread, so
  the value is attacker-influenced by whatever the VM stack holds; with a hostile stack layout it
  derefs an arbitrary handle. ASan on the `asan` preset should flag this as a
  stack-buffer-overflow / container-overflow on the operand stack.
- Proposed fix: add the missing guard, matching every sibling method:
  ```cpp
  if (name == "take") return bind("take", {"indices", "axis"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
      Args(vm, a, "take").require(1);
      std::vector<std::ptrdiff_t> idxs;
      ...
  ```
  No documented contract is at risk — `take(indices, axis = 0)` already declares `indices` as
  required in `inspectMembers()` (`stdlib_tensor.hpp:123`).
- Proposed test: `tools/tests/scripts/audit_tensor.ki` — assert `t.take()` throws a clean arity
  error, and a C++ case in `tools/tests/unit/test_tensor_autograd.cpp` (or `test_tensor.cpp`)
  asserting the message is stable. The test must FAIL on the unfixed build (today it produces a
  layout-dependent message, so assert on the *arity* text).

### A14-2: `Matrix.determinant()` / `ComplexMatrix.determinant()` leak a raw `tensor::TensorError` — the error loses its traceback and line:col  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/stdlib_matrix.hpp:220-224` (`determinant` bind) and
  `src/kirito/stdlib_complex.hpp:279-282` (`cpx::determinant`)
- What: the engine's `tensor::determinant` throws `tensor::TensorError` (a `std::runtime_error`), not
  a `KiritoError`. The sibling `inverse` translates it —
  `catch (const tensor::TensorError& e) { throw KiritoError(e.what()); }`
  (`stdlib_matrix.hpp:228-229`, `stdlib_complex.hpp:285-286`) — but `determinant` calls the engine
  bare. The escaping `TensorError` is only saved by the VM's blanket `std::exception` safety net, so
  it is *catchable*, but it arrives as a bare String with **no traceback and no line:col** — breaking
  CLAUDE.md's "errors carry line and column" contract that every other numeric error honours.
- Repro:
```ki
var matrix = import("matrix")
var inf = 1.0e308 * 10.0
discard matrix.Matrix([[inf,0.0],[0.0,1.0]]).inverse()      # vs .determinant()
```
```
--- inverse (translates -> KiritoError):
Traceback (most recent call last):
  File "/tmp/a14_det_u1.ki", line 3, in <module>
/tmp/a14_det_u1.ki:3:53: error: matrix contains a non-finite value (inf or NaN)

--- determinant (raw TensorError escapes):
/tmp/a14_det_u2.ki: error: matrix contains a non-finite value (inf or NaN)
```
The determinant form has **no `Traceback`, no `3:53`** — the user cannot see which call failed.
Same for `ComplexMatrix.determinant()` (`cmatrix det(inf) -> threw: matrix contains a non-finite
value (inf or NaN)`, also tracebackless).
- Impact: anyone whose matrix picks up an `inf`/`NaN` (an overflowing product, a `1/0` upstream)
  gets a location-free error and has to bisect by hand. The value path is common — `det` is the
  singularity test people reach for first.
- Proposed fix: route both through the same translator the module already uses. Best single-sourced
  as a small `mat::determinant(const MatrixVal&)` mirroring the existing `cpx::determinant`, with the
  try/catch that `cpx::inverse`/`mat` `inverse` already have — or simply reuse `tns::wrap`'s idiom.
  No documented contract at risk: the message text is unchanged, only the error *type* and the
  attached location.
- Proposed test: `tools/tests/scripts/audit_matrix.ki` / `audit_complex.ki` — assert
  `matrix.Matrix([[inf,0],[0,1]]).determinant()` throws and is caught by a **typed** `catch String as
  e` the same way `inverse` is; plus an `tools/tests/errors/` case pinning that the diagnostic
  carries `line:col`. Fails on the unfixed build (today the raw TensorError has no location).

### A14-3: `all`/`any` return their identity whole-tensor but **throw** over a zero-length axis — the same function disagrees with itself  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/stdlib_tensor.hpp:1193-1205` (`tns::allAny`), specifically line 1202
- What: `allAny`'s **whole-tensor** path (`axis < 0`) folds from a seed of `isAll`, so an empty tensor
  correctly yields the identity (`all() == True`, `any() == False` — NumPy semantics). Its
  **per-axis** path calls `tensor::reduceAxis(a, ax, comb)` with **no `identity` argument**, so
  `reduceAxis` hits its "no first element to seed and no identity to fall back on" branch
  (`tensor.hpp:331`) and throws. `all`/`any` have perfectly well-defined identities (True/False) —
  `reduceAxis` already takes the optional identity that `sum` (`0.0`, line 454) and `prod` (`1.0`,
  line 1807) pass. This is not the v1.15 documented rule: that rule is "sum/prod return their
  identity; **mean/min/max/std/var/ptp/median** THROW" — `all`/`any` are in neither list, and the
  whole-tensor path already commits to the identity answer.
- Repro:
```ki
var io = import("io")
var T = import("tensor")
var z = T.zeros([3, 0])
io.print("all() whole-tensor  ->", z.all())
io.print("any() whole-tensor  ->", z.any())
try:
    io.print("all(1) per-axis     ->", z.all(1))
catch as e:
    io.print("all(1) per-axis     -> threw:", e)
```
```
all() whole-tensor  -> True
any() whole-tensor  -> False
all(1) per-axis     -> threw: zero-size reduction: cannot reduce over an empty axis
any(1) per-axis     -> threw: zero-size reduction: cannot reduce over an empty axis
sum() / sum(1)      -> 0.0 [0.0, 0.0, 0.0]      <- sum agrees on both paths
prod() / prod(1)    -> 1.0 [1.0, 1.0, 1.0]      <- prod agrees on both paths
nonempty all(1)     -> [0.0, 1.0]               <- a non-empty axis is fine
```
NumPy gives `np.all(np.zeros((3,0)), axis=1) -> [True, True, True]` and
`np.any(...) -> [False, False, False]`.
- Impact: any per-axis `all`/`any` over data that can legitimately have a zero-length group (an
  empty filter result, a `split` that produced an empty section, a mask that selected nothing)
  throws instead of returning the identity — and the user cannot even predict it from the
  whole-tensor behaviour, which silently gives the opposite answer. This is exactly the class of
  "works on my data, throws in production" bug.
- Proposed fix: one line — pass the identity, as `sum`/`prod` already do:
  ```cpp
  FT out = tensor::reduceAxis(a, ax, [isAll](double x, double y) {
      bool xb = x != 0, yb = y != 0; return static_cast<double>(isAll ? (xb && yb) : (xb || yb)); },
      isAll ? 1.0 : 0.0);
  ```
  The comb already returns 0/1, so folding from the identity is numerically identical for a
  non-empty axis (`all`: `1 && e == e`; `any`: `0 || e == e`), and the following normalization line
  (added "for the single-length seed case") becomes redundant but harmless. **No documented contract
  is at risk** — the v1.15 throw-rule names mean/min/max/std/var/ptp/median, not all/any, and this
  makes the per-axis path agree with the whole-tensor path it already has.
- Proposed test: `tools/tests/scripts/audit_tensor.ki` — assert
  `T.zeros([3,0]).all(1) == T.Tensor([1,1,1])` and `T.zeros([3,0]).any(1) == T.Tensor([0,0,0])`,
  alongside the existing `sum(1)`/`prod(1)` identity assertions, plus a pin that the whole-tensor
  and per-axis paths agree. **Untested surface today** — `grep` finds only non-empty `all(1)`/`any(1)`
  cases (`r7_tensor.ki:294`, `r6_tensor.ki:268`); nothing covers the zero-length axis, which is why
  this survived.

### A14-4: `tensor.arange` allocates ~1 GB before rejecting an oversized range (allocate-then-check, unlike every sibling creator)  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/stdlib_tensor.hpp:2277-2281` (the `arange` kwfn)
- What: every other creation path checks the size **before** allocating — `zeros`/`ones`/`full`
  (`tns::checkSize(shape)` at 2201/2220), `eye` (2237), `linspace` (2334), `diag` (2363),
  `repeat`/`tile` (1071/1098). `arange` alone grows the buffer first and tests afterwards:
  ```cpp
  if (step > 0) for (double x = start; x < stop; x += step) { data.push_back(x); if (data.size() > tns::kMaxElems) throw KiritoError("Tensor too large"); }
  ```
  So it pushes 64M+1 doubles (512 MB, ~1 GB transient with `std::vector`'s growth doubling) purely
  to discover the request was too big. The element count is computable up front from
  `start`/`stop`/`step`, exactly as `linspace` does from `num`.
- Repro:
```ki
var T = import("tensor")
var inf = 1.0e308 * 10.0
try:
    var t = T.arange(0.0, inf, 1.0)
catch as e:
    io.print("threw:", e)
```
```
--- baseline VM:                                  9,408 kB   0:00.00
--- zeros([1e8]) (pre-checks the shape):         10,368 kB   0:00.00   "Tensor too large"
--- arange(0, inf, 1) (allocates then checks): 1,058,080 kB   0:01.53   "Tensor too large"
```
Rejecting an equivalently-oversized request costs **1 GB and 1.5 s via `arange` vs 1 MB and 0.00 s
via `zeros`** — a 100× memory difference for the same "no" answer.
- Impact: CLAUDE.md's contract is "Resource guards: huge string/list repetition, padding, and
  `range` are bounded (**throw instead of OOMing**)". A guard that first allocates 1 GB is a weak
  one: on a memory-constrained box it *is* the OOM it exists to prevent. It bites hardest under
  `parallel`, where N worker VMs each hold their own arena — a handful of workers hitting a
  bad `arange` simultaneously reserves multiple GB before any of them throws. Reachable from any
  computed bound (`arange(0, x/y)` where `y` underflows to 0 → `inf`).
- Proposed fix: compute the count first, then `checkSize`, keeping the existing in-loop guard as a
  backstop:
  ```cpp
  double span = (stop - start) / step;                    // NaN/inf-safe: the comparison below is false for NaN
  if (!(span <= static_cast<double>(tns::kMaxElems))) throw KiritoError("Tensor too large");
  ```
  placed right after the `step == 0` check. Non-finite `stop` makes `span` `inf` → rejected
  instantly; NaN makes the comparison false → also rejected (today NaN already yields an empty
  tensor via the loop condition, so keep that path if it is pinned — check `r4_tensor.ki`).
- Proposed test: `tools/tests/scripts/audit_tensor.ki` — assert `T.arange(0.0, inf, 1.0)` throws
  "Tensor too large"; the *value* assertion is already true today, so the regression test that
  matters is a C++ one in `tools/tests/unit/test_tensor.cpp` bounding peak allocation, or simply a
  timing assertion (the unfixed build takes ~1.5 s, a fixed one ~0 s).

## Log

(in progress)
