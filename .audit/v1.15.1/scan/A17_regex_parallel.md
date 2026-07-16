# A17 — regex engine + parallel/dispatcher (v1.15.1)

## Scope
- `src/kirito/regex_engine.hpp` (731 L) — recursive-descent parser → bytecode → Thompson-NFA Pike VM
- `src/kirito/stdlib_regex.hpp` (485 L) — the Kirito-facing `regex` module
- `src/kirito/dispatcher.hpp` (787 L) — `KiritoDispatcher`, worker VMs, cross-VM primitives
- `src/kirito/stdlib_parallel.hpp` (600 L) — the Kirito-facing `parallel` module

Contracts under test:
- **regex: LINEAR TIME.** Thompson NFA / Pike VM, no backtracking. A hang or superlinear blow-up IS a bug.
- **parallel: DEADLOCK-SAFE by construction.** `shutdown()` aborts every blocked primitive before joining.

Carried forward from v1.15 (do NOT "fix"):
- A14-1 `(a*)*` nullable-group capture differs from Python — accepted linear-time tradeoff, PINNED.
- A17-2 abort outranks a buffered Queue item; `close()` still drains — deliberate.
- `.audit/README.md` false positives: `Semaphore.release` over-release, `Lock.release` by non-owner — by design.

Probing with `./build-debug/ki`. Every parallel probe wrapped in `timeout`.

---

## Findings

(appended as confirmed)
