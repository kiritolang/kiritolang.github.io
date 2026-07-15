# A11 Tensor/Matrix/Complex

Status: IN PROGRESS

Scanner A11, v1.15 audit round. Subsystem: tensor.hpp, stdlib_tensor.hpp, stdlib_matrix.hpp, stdlib_complex.hpp.

Read .audit/README.md false-positive table first (left-scalar tensor throws, complex.polar negative rho,
Complex unhashable, exact ==, etc.) — will not re-flag those.

Initial observation: tensor.hpp itself carries inline comments referencing prior-round fixes (A11-1,
A13-1 already applied — checkedNumel/kMaxRank/kMaxElems guards, NaN-propagating min/max, broadcast-with-
zero-axis fix, non-finite-scale rejection in det/inverse). This file looks heavily hardened already from
earlier rounds. Continuing into stdlib_tensor.hpp (autograd), stdlib_matrix.hpp, stdlib_complex.hpp.

Read stdlib_tensor.hpp (full, 2519 lines), stdlib_matrix.hpp, stdlib_complex.hpp in full. Also ran a
live build (`build-debug/ki`, already configured) against ~20 adversarial one-liner probes covering:
empty-tensor sum, matmul shape mismatch, singular inverse (method-vs-module-fn), OOB index, tensor
division by zero, complex log(0), reshape-size mismatch, einsum bad/mismatched subscripts, backward on
a detached node, item() on non-scalar, ragged nested-list ctor, huge-shape guard, broadcast mismatch,
NaN propagation into determinant, 0-D tensor indexing/item, whole-tensor `==` with NaN, Complex tolist
round-trip, argmax of an empty tensor, zero-length-axis min/sum/mean, batched-matmul non-broadcastable
batch dims, non-contiguous (permuted) tensor arithmetic, setitem with a partial index, negative-axis
tensordot, out-of-range sort axis, std/var with ddof >= n, Complex-dtype requiresgrad rejection. Every
one of these produced a correct, clean, catchable `KiritoError` (or a correct numeric result) — no
crashes, no wrong answers, no silent corruption found in this pass.

This subsystem carries visible scar tissue from *five* prior audit rounds (comments cite v1.12 A13-1,
1.12.1 A11-1, v1.13 docinvariants, etc.) and reads as one of the most thoroughly hardened areas of the
codebase. The one substantive finding from this pass is A11-1 below (an inconsistency, not a crash).

