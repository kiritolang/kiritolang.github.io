# A13 — engines: regex + tensor + matrix + complex-matrix

Audit round v1.16.1. Findings appended incrementally.

Scope reviewed in full: regex_engine.hpp, stdlib_regex.hpp, tensor.hpp, stdlib_tensor.hpp,
stdlib_matrix.hpp, stdlib_complex.hpp. All repros run against build-asan/ki (ASan/UBSan clean).

Overall: this surface is exceptionally well hardened (linear-time regex confirmed, size/overflow
caps single-sourced through checkedNumel, empty-axis reductions guarded, grad-assign refused,
autograd numerically correct). Findings below are minor behavioral/consistency divergences, no
crashes or OOB found.

### F13-1 [Low] regex sub/split treat a NEGATIVE count/maxsplit as "unlimited" (Python does the opposite)
- stdlib_regex.hpp:327 (`sub` count) / :362 (`split` maxsplit) — `count = (int)args.asInt(...)`, then
  `allMatches(..., count > 0 ? count : -1)` and `if (maxsplit > 0 && splits >= maxsplit) break`.
  A negative value falls into the `<= 0` bucket and is treated as UNLIMITED (same as 0 = "all").
  Trigger / Verified-real: `regex.sub("a","X","aaa",-1)` -> "XXX"; `regex.split("a","bab",-1)` -> ['b','b'].
  Python's re does the reverse — a negative count/maxsplit performs ZERO operations
  (`re.sub("a","X","aaa",-1)` -> "aaa", `re.split("a","bab",-1)` -> ['bab']). The module docs itself
  "requests/Python-like", so a caller porting `count=-1` gets the opposite result silently.
  Fix idea: reject a negative count/maxsplit with a clear error (cleaner than matching either side),
  or match Python (negative => no ops). Not a crash. CONFIRMED.
- Test to add: tools/tests/errors or a golden .ki asserting the chosen semantics for negative count/maxsplit.

### F13-2 [Low] Partial integer indexing `t[i]` on a grad-tracking tensor detaches with NO warning
- stdlib_tensor.hpp:1621-1629 (getItem partial-index sub-tensor path) — every OTHER detaching read
  warns via warnDetach: `[]` slicing (:1559), boolean mask (:1581), fancy index (:1594). But a bare
  partial integer index `t[0]` on a rank>=2 tensor returns a detached sub-tensor with no warnDetach and
  no grad-aware alternative advertised.
  Trigger / Verified-real: `t = Tensor([[1,2],[3,4]], requiresgrad=True); t[0].requiresgrad()` -> False,
  and NO warning printed (whereas `t[0:1]` prints the standard detach warning).
  Fix idea: call `tns::warnDetach(vm, "[] integer indexing (use .take()/.slice() to keep gradients)", *this)`
  in the partial-index branch, for parity with the sibling read paths.
  Test to add: assert the warning fires (or that it is intentionally silent) for `t[i]` on a grad tensor.

### Notes (checked, NOT bugs — for maintainer confidence)
- Catastrophic patterns stay linear: `(a+)+$`, `(a*)*c`, `(x+x+)+y` on 28-40 char inputs run in <5ms;
  kMaxMatchWork (1e9) + numGroups<=1000 + insts<=200000 caps are all reachable-before-blowup.
- Empty-match iteration (findall/split/sub over `a*`, ``, `a|`) matches Python's must_advance output.
- Size explosion for outer/kron/matmul/concat/repeat/tile/diag/eye all route through checkedNumel or an
  explicit pre-multiply checkSize; the products can't wrap size_t before the cap (kron's `ar*br` <= 4e15
  fits, then the {..,..} shape trips checkedNumel).
- Empty-axis argmin/argmax/std/median/all/any: the whole-tensor `a.data.empty()` guard fires first
  (any zero axis => total numel 0), so the per-line `a.data[base]` seed is never reached OOB. ASan-clean
  on `zeros([3,0]).argmax(axis=1)` etc.
- Grad-tracking element assignment refused; div/mod/floordiv by a zero scalar/element throw; matmul dim
  mismatch, non-square det/inv/trace, singular inverse, cross non-3-vector all throw clean KiritoErrors.
- Autograd verified numerically: d/dx sum(x^2+3x) = 2x+3 -> [5,7,9]; matmul grad correct; einsum
  diagonal/trace correct; backrefs/lookaround/lookbehind cleanly rejected at parse time.
- _setstate_ paths (Tensor/Matrix/ComplexMatrix) validate dtype, reject negative dims, and the tensor
  ctor's `data.size()==checkedNumel` recheck blocks a size/shape mismatch from untrusted blobs.

