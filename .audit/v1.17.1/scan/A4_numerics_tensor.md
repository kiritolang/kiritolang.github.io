# A4 — Numeric-correctness / UB audit: BigInt, math, complex, matrix, hashing, tensor engine

Scope: `stdlib_int.hpp` (BigInt), `stdlib_math.hpp`, `stdlib_complex.hpp`, `stdlib_matrix.hpp`,
`hashing.hpp`, `hashmix.hpp`, `tensor.hpp`, `stdlib_tensor.hpp` (all non-slicing tensor math).
Method: reproduced on `build-bin/ki-release` and `build-bin/ki-asan` (ASAN_OPTIONS=detect_leaks=0,
UBSan-linked); every value cross-checked by hand or against Python's `math`/`cmath`/`hashlib`/`hmac`
(no numpy present, so tensor results were hand-computed). No source or tests were modified.

Probe convention: `var io=import("io"); io.print(...)`, `True`/`False` literals, no semicolons.

---

## FINDINGS

### F1 — Tensor `%` uses truncated `fmod`, not floor-mod: wrong result on negatives, breaks the divmod identity  — CONFIRMED, severity HIGH

- **Where:** `src/kirito/stdlib_tensor.hpp:1063-1066` (`tns::ewFloat`) and `:1068-1072`
  (`tns::ewFloatScalar`). The `%` branch is `std::fmod(x, y)` with **no** floor-mod sign correction,
  while the paired `//` branch is `std::floor(x / y)`.
- **Contrast (the single source of truth it diverges from):** the scalar Float `%`
  (`src/kirito/runtime.hpp:281-289`) computes `r = fmod(x,y); if (r!=0 && (r<0)!=(y<0)) r += y;` —
  i.e. floor-mod. The BigInt/Integer `%` also floor. And the language contract is *documented*:
  `docs/pages/09-types.md:65` — "`//` (floor division), `%` (modulo) … floor toward negative infinity";
  `docs/pages/02-language-guide.md:95` — "`%` is paired with `//`". NumPy's array `%` is `np.mod`
  (floor), not `np.fmod`. Tensor `%` is the only `%` in the language that does not floor.
- **Reproducer:**
  ```
  var t=import("tensor")
  io.print(-7.0 % 3.0)                                  # scalar  -> 2.0   (floor-mod, correct)
  io.print((t.Tensor([-7.0]) % t.Tensor([3.0])).tolist())  # tensor -> [-1.0]  (fmod, WRONG)
  io.print((t.Tensor([-7.0]) % 3.0).tolist())              # tensor -> [-1.0]  (WRONG, expected [2.0])
  io.print((t.Tensor([7.0]) % -3.0).tolist())              # tensor -> [1.0]   (WRONG, expected [-2.0])
  ```
  Actual vs expected (correct = floor-mod, what scalar `%` / NumPy / the docs give):
  - `-7 % 3`: tensor gives **-1.0**, correct is **2.0**.
  - `7 % -3`: tensor gives **1.0**, correct is **-2.0**.
- **Divmod identity broken** (the exact property `docs/pages/09-types.md:65` promises holds):
  ```
  var a=t.Tensor([-7.0]) ; var b=t.Tensor([3.0])
  io.print(((a//b)*b + (a%b)).tolist())   # -> [-10.0], must equal a = [-7.0]
  ```
  `//` floors to -3 but `%` truncates to -1, so `(-3)*3 + (-1) = -10 ≠ -7`. With floor-mod (`%`==2)
  the identity gives `-9 + 2 = -7`. CONFIRMED on ki-release.
- **Root cause:** classic "two implementations of the same operator diverge" — the tensor path was
  written with raw `std::fmod` and never given the floor sign-correction its scalar sibling and its
  own `//` partner have. Positive/positive operands (the only case in the doc example
  `[5,7,9] % 3`) hide it; any negative operand exposes it.
- **Fix direction (not applied — audit only):** make the tensor `%` element op floor-mod, e.g.
  `r = std::fmod(x,y); if (r != 0.0 && ((r < 0.0) != (y < 0.0))) r += y;` — copy the runtime.hpp
  logic so the two are single-sourced.

---

## Verified CORRECT (independently checked values)

### BigInt (`stdlib_int.hpp`) — all correct
Checked with **string** inputs (note: a decimal *literal* > 2^63 wraps two's-complement in the
lexer — `parser.hpp:1118-1142`, documented/intentional, out of numeric-module scope; use
`big("...")`/`fromstring` for exact large values).
- Floor `//`/`%` with mixed signs: `-7//2,-7%2 = -4,1`; `7//-2,7%-2 = -4,-1`; `-7//-2,-7%-2 = 3,-1`. ✓
- `factorial(30) = 265252859812191058636308480000000`; `factorial(100)`, `comb(100,50) =
  100891344545564193334812497256`, `perm(20,10)=670442572800`. ✓
- `modpow(2,1000000,1000000007)=235042059`; `2^63=9223372036854775808`; `7^256 mod 13 = 9`. ✓
- `isqrt(10^38)=10^19`, `isqrt(10^21)=31622776601` (via string). ✓
- `gcd(q,p)`/`q%p`/`q//p` on 30-digit values = `9000000000900000000090` / `8` (hand/py). ✓
- `modinv(17,1000000007)=352941179`, `modinv(3,11)=4`; non-coprime `modinv(4,8)` throws. ✓
- `fromstring("-deadbeef",16) = -3735928559`; base round-trips. ✓
- `INT64_MIN.toint()` works; `INT64_MAX±1` exact; reflected-op `3+big(2)` throws (left-dispatch rule). ✓
- Div/mod/modpow by zero, `isqrt(-1)`, `factorial(-1)`, negative modpow exponent all throw. ✓
- Ran under ki-asan+UBSan at INT64 extremes: **no UBSan trip, no ASan report.** ✓
- `big("9223372036854775808")==2^63-as-Float` returns False: an accepted, documented false-negative
  (equality only exact within int64 range; `stdlib_int.hpp:542-546`). ✓

### math (`stdlib_math.hpp`) — all correct
`comb(64,32)=1832624140942590534`, `perm(20,10)`, `gcd(-12,18)=6`, `lcm(-4,6)=12`, `log(8,2)=3`,
`sqrt(nan)=nan` (passes through), `floor(-2.5)=-3`/`ceil(-2.5)=-2`, `prod([2,3,4])=24`,
`prod([1e12,1e12,0])=0` (zero clears overflow), `gamma(0.5)=√π`, `hypot(3,4)=5`. The `comb`
128-bit intermediate divide is exact (partial binomial is integral). ✓

### complex (`stdlib_complex.hpp`) — all correct
`sqrt(-1)=i`, `log(-1)=iπ`, `exp(iπ)=-1` (+1.2e-16i rounding), `|3+4i|=5`, `arg=0.92729…`,
`(i)^2=-1`, Hermitian dot uses `conj(a)·b`, ordering (`<`) throws (unordered), div-by-zero throws,
`polar` rejects non-finite. Determinant/inverse via the shared engine (see tensor). ✓

### matrix (`stdlib_matrix.hpp`) — correct
Thin wrapper over the tensor engine (`add/sub/matmul/scalarOp/transpose/determinant/inverse/
trace/sumAll`), with element-count caps and non-negative-dim guards on `_setstate_`. Covered by the
tensor engine checks below. ✓

### tensor engine (`tensor.hpp` + `stdlib_tensor.hpp`) — all correct except F1
- **Broadcasting** `(2,1)+(1,3)`, incompatible-shape throws, 0-length axis wins over 1. ✓
- **matmul** 2×3·3×2 = `[[4,5],[10,11]]`; batched (2,2,2)·(2,2,2) per-batch correct; zero inner dim
  `(2,0)·(0,3)` → 2×3 zeros. ✓
- **Reductions**: `sum(0/1)`, whole `sum=21`, `mean`, `max(0)`/`min(1)`, `prod(1)=[6,120]`;
  `var(1)=0.6667`, `std(1,ddof=1)=1`; empty-axis `sum=0`/`prod=1`/`all=1`/`any=0` (identities);
  1-D reduce over axis 0 → 0-D scalar shape `[]`. ✓
- **NaN/Inf propagation**: `max/min/sum` of `[1,nan,3]` all `nan` (numpy amax/amin semantics,
  position-independent); `argmax` returns first-NaN index; `[1,inf,-inf]` → max=inf,min=-inf,sum=nan. ✓
- **Autograd vs analytic** (all exact): d(Σx²)=2x, d(Σeˣ)=eˣ, d(Σlog x)=1/x, matmul grads
  (dA=g·Bᵀ, dB=Aᵀ·g), broadcast grads sum-to shape (`aa.grad=[[60],[60]]`, `bb.grad=[[3,3,3]]`),
  x³→3x², mean→1/n, sqrt→0.5/√x, sigmoid(0)→0.25, sigmoid(2)→0.105, tanh(0.5)→0.7864,
  a/b grads, relu/abs subgradient 0 at 0, reshape/concat/stack grads. Iterative backward (no native
  recursion), non-leaf grad reset each pass. ✓
- **einsum** `ij,jk->ik` (=matmul), `ii->` (trace=5), `ij->ji`, `ii->i` (diag), `i,j->ij` (outer);
  repeated/undeclared output labels and overflow are rejected. ✓
- **tensordot/contract** axes=2 full contraction=5; `contract(x,I,[2],[0])` = identity. ✓
- **linalg**: `det([[4,7],[2,6]])=10`, `inv=[[.6,-.7],[-.2,.4]]`, cross(e1,e2)=e3, kron, outer,
  `norm(2)=5`, `norm(1)`, `norm(3)=17^(1/3)`; complex matmul (1+i)²=2i, complex det=2. ✓
- **dtype promotion** Float⊕Complex → Complex with reals on the real axis; `ft*ct` etc. correct. ✓
- **misc**: clip, maximum/minimum, where, median (even=avg, odd=mid, NaN sorts last), cumsum/cumprod
  and cumsum grad, sort/argsort/unique NaN-last, sign, reciprocal, round (half-to-even via
  `nearbyint`), astype, arange/linspace(num=1→start)/repeat/tile, `//` floors correctly. ✓
- Division-by-zero (`/`, `%`, `//`, reciprocal-of-0) and out-of-domain math ops (log/sqrt/asin/…)
  throw instead of emitting silent NaN; setItem on a grad tensor hard-errors. ✓
- Ran the reduction/grad/concat battery under ki-asan+UBSan: **no ASan/UBSan report.** ✓
- Element-count/stride math is guarded by `checkedNumel` (running-division overflow check,
  `kMaxElems=64M`, `kMaxRank=64`) at the single allocation point; einsum guards its own
  `total` product against `SIZE_MAX` overflow. No overflow reachable from the probes. ✓

### hashing (`hashing.hpp`, `hashmix.hpp`) — all correct vs known vectors
`md5("")`, `sha1("abc")`, `sha256("abc")`, `sha512("")`, `sha384("abc")`,
`hmac-sha256(key, "The quick brown fox…")`, `pbkdf2-sha1("password","salt",1,20)` — every digest
matches Python `hashlib`/`hmac` byte-for-byte. SipHash-2-4 (bucket mixing) is the reference impl
with correct tail handling; `pbkdf2` rejects `iterations < 1`. ✓

---

## Nulls / non-findings (honest notes)
- The decimal-literal two's-complement wrap (`1000000000000000000000` → wrapped int64) is a **lexer**
  behavior, documented and intentional (`parser.hpp:1118-1123`), not a numeric-module bug. It *is* a
  silent wrong value if a user writes a huge literal expecting BigInt; flagged here only for awareness.
- BigInt≡Float equality is a documented false-negative outside int64 range — intentional.
- Tensor `prod`/sort/median/etc. are non-differentiable and `warnDetach` on grad tensors — a
  documented limitation, not a numeric error.
- No numpy on the box; all tensor expectations were hand-computed. Confidence is high for the checked
  cases but the tensor surface is large — F1 was found by systematically pairing every operator with
  its scalar sibling, which is the most productive divergence-hunting method and may be worth
  repeating for any op added later.
