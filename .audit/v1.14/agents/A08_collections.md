# A08 — Collections Audit (v1.14) — List / Set / Dict / Array + hashing

Auditor: A08 (read-only static audit). Scope: `src/kirito/collections.hpp`,
`src/kirito/hashing.hpp`, and List/Set/Dict/Array method + operator + iteration
implementations in `src/kirito/runtime.hpp`.

NOTE: Task named `list_value.hpp`/`set_value.hpp`/`dict_value.hpp`/`array_value.hpp` —
these do NOT exist. Actual layout: `collections.hpp` (Val classes) + method surface in
`runtime.hpp` + `hashing.hpp`. Matches v1.13 layout.

Prior v1.13 findings (A09): A09-1 List value-search UAF via reentrant `_eq_` (High),
A09-2 NaN sort UB, A09-3 unhashable-in-dict-vs-set asymmetry, A09-4 empty-bucket leak,
A09-5 subset family unrooted, A09-6 DRY + phantom Array kind. Checking which merged.

Status: IN PROGRESS.

---

## Findings

