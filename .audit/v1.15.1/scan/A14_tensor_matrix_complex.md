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

### A14-5: in-place element assignment `t[i,j]=v` silently desyncs the autograd graph — `backward()` then returns gradients computed against stale pre-mutation values, with no error  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/stdlib_tensor.hpp:1625-1639` (`TensorVal::setItem`).
- What: `setItem` writes the new element straight into `store` (`std::get<FT>(store).at(idx) = ...`)
  and never touches the tensor's autograd state (`requiresGrad`, `node`, or any cached forward copy).
  The differentiable ops snapshot their inputs at forward time (e.g. `g_math` copies `acopy = a`,
  `g_pow` copies `acopy = a`, `g_mul`/etc. capture the operand data into the closure), so a later
  in-place edit of the SAME tensor is invisible to the recorded graph. `backward()` then produces
  gradients for the values that existed **when the op ran**, not for the tensor's current contents —
  and the visible tensor data and its graph silently disagree. PyTorch guards exactly this: an
  in-place op on a leaf requiring grad is a hard error, and an in-place edit of a tensor still needed
  for backward is caught by version counters at `.backward()`. Kirito has neither guard.
- Repro (real output, `/tmp/a14/p6.ki`):
```ki
var w = tensor.Tensor([1.0,2.0,3.0], requiresgrad=True)
var y = w * w          # graph built with w = [1,2,3]
w[0] = 100.0           # mutate the leaf AFTER the graph is built
var loss = y.sum()
loss.backward()
io.print("w now:", w.tolist())      # -> [100.0, 2.0, 3.0]
io.print("grad:", w.grad)           # -> [2.0, 4.0, 6.0]   (= 2*[1,2,3], the STALE values)
```
```
w now: [100.0, 2.0, 3.0]
grad (2*w? which w?): [2.0, 4.0, 6.0]
y: [1.0, 4.0, 9.0]
mutated non-leaf b: [999.0, 4.0]
grad after mutating non-leaf: [2.0, 2.0]
```
  `w.grad` is `[2,4,6]` while `w` is now `[100,2,3]` — the gradient of `sum(w*w)` at the current leaf
  should be `[200,4,6]`. Second case: mutating a **non-leaf** tracked result `b = a*2` to `b[0]=999`
  leaves `b.tolist()==[999,4]` yet backward computes as if `b==[2,4]` (grad w.r.t. `a` = `[2,2]`), so
  the tensor's own value and its graph are inconsistent with no diagnostic.
- Impact: any code that mixes the one supported in-place op (`t[i,j]=v`, documented as the single
  mutator) with autograd gets silently wrong gradients — no throw, no NaN, just a wrong number. It is
  reachable from ordinary scatter/masking-style updates inside a training loop (`params[i] = ...`).
  The functional-update design (rebind, don't mutate) *avoids* it, but nothing enforces that design,
  so a user who mutates a grad tensor gets a plausible-looking wrong answer. This is the
  "two supported features interact wrongly, silently" class.
- Proposed fix: make `setItem` refuse to mutate a tensor that participates in a graph — throw a clear
  error when `requiresGrad` is set or `node` is non-null (mirroring PyTorch's leaf guard), e.g. right
  after the index checks:
  ```cpp
  if (requiresGrad || node)
      throw KiritoError("cannot assign into a Tensor that tracks gradients; detach() first "
                        "(in-place edits are invisible to the autograd graph)");
  ```
  This keeps the documented "element assignment is the only in-place op" true for plain tensors while
  closing the silent-wrong-grad hole. (A softer alternative — invalidate the graph on mutation — is
  more work and still surprises the user; the hard error matches the rest of the numeric stack's
  "throw, don't silently mislead" stance.)
- Proposed test: `tools/tests/scripts/audit_tensor.ki` — assert that assigning into a
  `requiresgrad=True` tensor throws (typed `catch`), and a C++ case in
  `tools/tests/unit/test_tensor_deep.cpp` pinning the message. **Untested surface today** — the 273
  `requiresgrad` test lines never combine autograd with `t[i]=v`.

### A14-6: `softplus` forward overflows to `inf` for large inputs (naive `log1p(exp(x))`), unlike NumPy/PyTorch's numerically-stable formula — breaks training on unnormalized activations  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/stdlib_tensor.hpp:532` (`mathForward`, `MathOp::Softplus`):
  `return std::log1p(std::exp(x));`
- What: for `x` past ~709, `std::exp(x)` overflows to `+inf`, so `softplus(x)` returns `inf` even
  though the true value is ≈`x` (softplus is asymptotic to the identity). NumPy/`scipy`/PyTorch use
  the stable identity `softplus(x) = max(x,0) + log1p(exp(-|x|))`, which never overflows and returns
  ≈`x` for large `x`. The **derivative** here is already stable (`sigmoid(x)`, line 532's neighbour),
  so only the forward pass is wrong — which is worse, because it silently poisons the loss.
- Repro (real output, `/tmp/a14/p7.ki`):
```ki
io.print("softplus 1000:", tensor.Tensor([1000.0]).softplus().tolist())
```
```
softplus 1000: [inf]
```
  (True value ≈ 1000.0.)
- Impact: `softplus` is in the documented differentiable math set and is used as an activation in the
  `deep_learning` / `kgrad` examples. A single large pre-activation (an unnormalized input, an
  exploding layer) turns the forward output — and hence the loss — into `inf`, then `nan` on the
  backward multiply, killing the run with no clue that softplus was the culprit. Sigmoid, by
  contrast, saturates cleanly (`sigmoid(-1000)` → `0`, no inf), so this is specific to softplus.
- Proposed fix: the standard stable form:
  ```cpp
  case MathOp::Softplus: { double m = x > 0.0 ? x : 0.0; return m + std::log1p(std::exp(-std::fabs(x))); } break;
  ```
  Value-identical to today for small `x` (still `log1p(exp(x))` at `x=0` → `0.6931…`, the pinned
  case), overflow-free for large `x`. No documented contract changes.
- Proposed test: `tools/tests/scripts/audit_tensor.ki` — assert `T.Tensor([1000.0]).softplus()`
  ≈ `1000.0` (finite), alongside the existing `softplus(0)` case. **Untested surface today** —
  `r4_tensor.ki:418` / `r6_tensor.ki:448` only check `softplus(0.0)`, never a large input.

## Coverage gaps

- **Autograd × in-place** (A14-5): no test combines `t[i]=v` with a grad-tracking tensor. Broader:
  no test drives autograd through `flip`/`squeeze`/`expanddims`/`swapaxes`/`broadcastto` grad paths
  (I verified reshape/concatenate/where/broadcast grads are correct, but the structural-op grads are
  unverified here).
- **Numerical stability** beyond softplus: `sigmoid` is stable; `softplus` is not (A14-6). Did not
  exhaustively probe `erf`/`tanh`/`log1p` edge overflow — likely fine but untested at extremes.
- **NaN-in-argmax/argmin per-axis**: whole-tensor NaN argmax is deliberate + documented (A13-1); the
  per-axis NaN path (`forEachLine` branch, `stdlib_tensor.hpp:1157-1164`) mirrors it but I did not
  find a test exercising a NaN inside a per-axis argmax.
- **`einsum`**: only lightly probed (2-operand). Did not fuzz malformed subscript strings.
- **`matrix`/`complex` module DRY**: the two determinant-error leaks (A14-2) are the one confirmed
  Float-vs-Complex/tensor-vs-matrix divergence; the rest of the shared surface (transpose/inverse/
  trace/apply/dot/cross/norm) is symmetric and correctly guarded in both.

## Non-findings (verified correct, with the inputs used)

- **Purity**: `x + y`, `x * y`, broadcast `+`, `matmul` all leave operands unchanged (`/tmp/a14/p1.ki`,
  `p7.ki`). Only `t[i,j]=v` mutates — as documented (but see A14-5 for its grad interaction).
- **Broadcasting** `(2,1)+(1,3)→(2,3)` correct values + grad `[[60],[60]]` (`p1.ki`); `where`
  broadcasts a `(1,1)` arg correctly (`p7.ki`).
- **`==` bit-exact / `.compare` tolerant** holds for Tensor, Matrix, ComplexMatrix, Complex:
  whole-tensor `==` with NaN → `False`, `.compare` NaN → `False`, `[-0.0]==[0.0]` → `True`,
  near-equal matrices `==`→`False` but `.compare`→`True`, `Complex(2,0)==2`→`True` (`p2.ki`, `p5.ki`).
  No method found where `==` is wrongly tolerant or `.compare` wrongly exact.
- **Domain errors THROW** across the analytic set: tensor `log`/`sqrt`/`asin`/`acos`/`acosh`/`atanh`
  of out-of-domain elements, `reciprocal(0)`, scalar `t/0`, `t%0`, `t**0.5` of a negative base, and
  the complex `log(0)`/`atanh(±1)`/`pow(0,-1)` all throw a clean `math domain error`; `sqrt(-1)`/
  `asin(2)`/`cbrt(-8)` correctly do NOT throw (`p7.ki`, `p9.ki`).
- **Empty / 0-dim / singular**: `sum`/`prod` of an empty tensor → identity, `mean`/`max` → throw;
  0-dim scalar tensor `item()`/arithmetic work; singular matrix `inv`/`solve`/`matrix.inverse` throw
  "matrix is singular", `det`→`0.0` (`p3.ki`).
- **Per-axis reductions over a zero-length axis** are self-consistent for `mean`/`max`/`min`/`std`/
  `var`/`ptp`/`median` (all THROW, matching their whole-tensor path + the documented rule);
  `cumsum(1)` → `[[],[],[]]` (`p4.ki`). The lone outlier is `all`/`any` — already recorded as A14-3.
- **`std`/`var` with `ddof >= n`** throws "not enough elements" rather than dividing by ≤0 (`p4.ki`).
- **NaN in reductions**: `min`/`max` propagate NaN, `sort` puts NaN last, `argmax` returns the NaN
  index — all deliberate/documented (A13-1) (`p8.ki`).
- **Backward semantics**: non-scalar `backward()` without a seed throws "seed gradient is required";
  repeated `backward()` accumulates into `.grad` (PyTorch-style) (`p1.ki`).
- **Mixed Float/Complex promotion**: `concatenate`/`stack`/`matmul`/`+` promote to Complex; `where`
  and elementwise `.eq()`/`<` are Float-only (documented limitations, not bugs) (`p5b.ki`, `p11.ki`).
- **Arg validation**: `reshape` total-mismatch, `squeeze` non-1 axis, `expanddims`/`sum(axis)` out of
  range, `clip(lo>hi)`, `matmul` rank<2, negative `split` sections all throw cleanly (`p10.ki`,
  `p8.ki`). `reshape([2,-1])` (NumPy -1 inference) is unsupported — a documented `readShape` rejection,
  not a bug.
- **OOB span audit (A14-1 class)**: I re-swept every fixed-arity instance method in all three files.
  Every one except `take` (A14-1) guards `a[0]` with `Args(...).require(N)`, an `a.empty()`/`a.size()`
  check, or a signatured `NativeFunction` (bindArgs-padded). All module-level functions are either
  `m.fn` (signatured/padded) or `m.kwfn` (explicit `a.size()` guards). **`take` remains the sole
  span-overread.**

## Status: DONE
