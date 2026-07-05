# A15 — Tensor + N-dim Engine + Autograd Audit

**Agent:** A15
**Area:** `src/kirito/stdlib_tensor.hpp` (2487 lines), `src/kirito/tensor.hpp` (586 lines)
**Tests:** `tools/tests/unit/test_tensor.cpp`, `test_tensor_deep.cpp`, `test_multi_index.cpp`, scripts in `tools/tests/scripts`
**Method:** static reasoning, read-only. Findings below: confirmed bugs first, then coverage gaps.

---

## Findings

