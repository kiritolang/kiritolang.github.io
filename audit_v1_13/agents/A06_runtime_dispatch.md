# A06 — Runtime dispatch: operators, call protocol, member access, numeric semantics

Area: `src/kirito/runtime.hpp` (~3529 lines). Focus: `applyBinaryOp`/`applyUnaryOp`, `numericBinary` + Integer/Float fast path, comparisons + `in`/`not in`, call protocol (`applyCall`/`callKw`/`makeMethod`, kwarg binding, defaults at call time, enforcing annotations), member get/set (`evalMemberGet`/`evalMemberSet`), privacy check, `self._super_()` parent view. Excludes per-type method tables (A09/A10) except how methods are dispatched/bound.

Status: IN PROGRESS.

---

## Findings

