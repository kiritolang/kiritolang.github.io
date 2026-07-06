# A04+A05 — Bytecode VM + Runtime Dispatch (v1.14)

Audit of `src/kirito/bytecode_vm.hpp` and `src/kirito/runtime.hpp`.
Builds on v1.13 A05 (bytecode_vm) + A06 (runtime dispatch). Focus: what they missed + re-verify their fixes.

Status: IN PROGRESS.

---

## Re-verification of v1.13 fixes

- **A05-1 (GetIter GC-UAF)** — FIXED, code-correct. `bytecode_vm.hpp:304` now declares
  `RootScope rs(vm_)` before the `if (lazy)` branch (hoisted out of the old `else`), the eager branch
  adds every produced item to `rs` (line 312), and `alloc(cursor)` (line 315) executes while `rs` is
  still alive. Since `alloc` runs GC before insertion, the items are now rooted through that window.
  Correct and complete. (Full ASan GC-stress confirmation requires an asan build + `setGcThreshold(1)`,
  which is not scriptable from `ki` — flag for orchestrator to run the proposed A05-1 test under asan.)
- **A09-2 (NaN sort total order in kiLessThan)** — FIXED, code-correct + empirically confirmed.
  `runtime.hpp:403-409`: `x<y iff (isNan(y) && !isNan(x))`, imposing NaN-as-largest total order.
  Verified: `[3.0,nan,1.0,inf,2.0,nan].sort()` -> `[1.0,2.0,3.0,inf,nan,nan]`;
  `min([nan,1.0,2.0])==1.0`; `max([1.0,nan,2.0])==nan`. Strict-weak-ordering holds (NaN equivalent
  only to NaN; distinct from every real incl. +inf). Correct.
- **Bytes ordering branch in kiLessThan** — FIXED, code-correct + empirically confirmed.
  `runtime.hpp:416-418`: `dynamic_cast<BytesVal>` on both, compares `xb->data < yb->data`
  (std::string unsigned-byte memcmp order). Verified sort/min/max over Bytes yields unsigned-byte
  lexicographic order (`[b'\x00', b'\x80', b'\xff']`). Correct.

---

---

## Findings

(pending)
