# Discrepancies — `tensor` module (docs/pages/10-stdlib.md `## tensor` + 30-bonus-05-tensors.md
# vs src/kirito/stdlib_tensor.hpp, tensor.hpp)

**Result: no discrepancies found.** The entire documented surface behaves as described. Verified by
`tools/tests/scripts/verify_tensor.ki` (225 assertions, `.expected` = `OK tensor`).

## Coverage

- **Constructors/factories:** `Tensor(data[,dtype][,requiresgrad])` (+ `tensor` alias), bare-Number
  rank-0 scalar, `zeros`/`ones`/`full`/`eye`/`arange`(stop / start,stop / start,stop,step / kw)/
  `linspace` (+ default num=50). dtype `Float` (default) and `Complex`. Empty shapes (`zeros([0])`,
  `zeros([2,0])`).
- **Indexing/assignment:** full index → scalar, partial → sub-tensor, negative indices, `t[i,j]=v`
  (+ negative), `t[a:b:c]` first-axis slicing (negative bounds, `::-1`), boolean mask, fancy `t[[i,j]]`,
  autograd-aware `.slice()`/`.take(axis)`.
- **Arithmetic:** `+ - * / % // **`, unary `-`, scalar ops, broadcasting `[2,1]+[2]→[2,2]`, PURITY
  (operands unmutated).
- **`==` vs `.eq`:** whole-tensor `==`/`!=` return a `Bool` (exact, shape-aware); `.eq/.ne/.lt/.le/
  .gt/.ge` and `< <= > >=` operators return a 0/1 mask Tensor; `.compare(other, rel_tol, abs_tol)`
  tolerant whole-tensor.
- **Logic/selection:** `logicaland/or/xor/not`, `where`, `clip`, `maximum`, `minimum`.
- **Linear/products:** `matmul` (2-D + batched), `dot` (plain Float), `tensordot` (axes int / pair),
  `contract`, `inner` (0-D Tensor), `transpose`/`permute`/`reshape`/`flatten`/`apply`.
- **Reductions:** `sum/mean/prod/min/max/argmin/argmax/std/var(ddof)/all/any/ptp/median/cumsum/
  cumprod`, whole + per-axis + negative axis.
- **Structural:** `squeeze/expanddims/swapaxes/flip/broadcastto/repeat/tile/concatenate(+concat)/
  stack/split` (int + sizes).
- **Creation helpers:** `zeroslike/oneslike/fulllike/identity/diag(1-D↔2-D)/tril/triu`.
- **Linalg module fns:** `det/inv/solve/trace/norm(ord)/outer/inner/kron/cross/einsum` (trace/
  transpose/matmul, repeated-output-label rejection).
- **Sort/search:** `sort/argsort/unique/nonzero/searchsorted` (scalar→Integer, tensor→Tensor).
- **Complex helpers:** `real/imag/conj/angle` (incl. `angle()` on Float → π for negatives).
- **Element-wise math:** `exp/log/sqrt/square/abs/sign/floor/ceil/relu/sigmoid/reciprocal/sin/cos`
  (tolerant `.compare` for irrationals); overflow-to-inf is NOT a domain error.
- **Autograd:** `requiresgrad()` get/set, `grad` (None pre-backward), `backward([seed])`, gradient of
  `sum(x*x)=2x`, accumulation across backward, `zerograd`, seeded non-scalar backward, matmul grad,
  `detach()`, `with nograd():` (+ restore), the **double-backward-through-a-retained-intermediate**
  fix (non-leaf grad is cleared each pass, so a 2nd backward is not doubled).

## Hardening verified (exact-message `raises_msg`)

- Constructors: ragged nested list (`ragged`), bad dtype (`Float`/`Complex`), `eye(-1)`
  (`non-negative`), negative dim.
- Indexing: OOB (`out of range`), too many indices (`too many indices`), non-Integer index
  (`must be Integer`), slice 0-D (`0-D`), slice step 0 (`step`), mask shape (`mask`), partial
  assignment (`full index`).
- Arithmetic: shape mismatch (`broadcast`), `/`,`%`,`//` by zero (`division by zero`/`modulo by
  zero`/`floor-division by zero`) for both scalar and tensor divisors; complex ordering (`unordered`).
- Products: matmul rank<2 (`rank`), matmul inner mismatch (`inner dimensions differ`), dot length
  (`equal length`), tensordot non-tensor (`Tensor`), reshape element-count (`unchanged`).
- Reductions: bad axis (`axis out of range`), argmax on empty (`empty`).
- Structural: squeeze non-1 (`size is not 1`), broadcastto incompatible/source-larger
  (`broadcast`/`cannot broadcast`), split indivisible (`divisible`).
- Linalg: det/inv/trace non-square (`square`), singular inv (`singular`), einsum repeated output
  label (`more than once`), cross non-3-vector (`3-element`).
- Element-wise math domain errors (mirror scalar `math`): `sqrt(-1)`, `log(0)`, `asin(2)`,
  `acosh(0.5)`, `atanh(1)`, `reciprocal(0)`, `pow(-2,0.5)`, `pow(0,-1)` — all `math domain error` /
  `division by zero` / `negative power`.
- Autograd: complex `requiresgrad` (`Float-only`), non-scalar backward without seed (`seed`), seed
  shape mismatch (`shape must match`).

## Silent-error probes

No case that should raise was found to silently succeed. Notable correct-but-subtle behaviours
(documented, so NOT discrepancies):

- Whole-tensor `sum()`/`mean()` return a plain `Float` on a non-grad tensor but a **0-D Tensor** on a
  grad-tracking tensor (so the graph continues) — documented in `10-stdlib.md`.
- `broadcastto` to an incompatible shape surfaces the engine's `tensors are not broadcastable to a
  common shape`; the wrapper's own `cannot broadcast to the requested shape` only fires when the
  source is broadcast-compatible but larger than the target (both paths correctly throw).
- Non-differentiable ops on a grad tensor emit a one-time stderr detach warning (not an error); these
  are expected and do not affect stdout / `.expected`.

No `src/` edits made (per audit rules).
