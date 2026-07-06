# clang-tidy static-analysis triage (v1.14)

Ran `clang-tidy` 18.1.3 over the whole interpreter (umbrella `kirito.hpp`, `--header-filter=kirito/.*`)
with `bugprone-*`, `cppcoreguidelines-narrowing/member-init`, `performance-*`, `portability-*`,
`clang-analyzer-*`. Raw output: `clang-tidy-headers.txt` (115 warnings). **No smoking-gun bug.**

## Correctness-relevant (investigated)

| Site | Check | Verdict |
|------|-------|---------|
| `bytecode_vm.hpp:313` | unchecked-optional-access | **False positive** — guarded by `if (!items) throw` at :310 (the A05-1 GC-rooting fix). `.value()` is safe. |
| `net_compat.hpp:160`, `stdlib_net.hpp:235` | uninitialized `struct timeval tv` | **False positive** — `tv.tv_sec`/`tv.tv_usec` are both assigned before use in `setTimeout`/`connectWithTimeout`. |
| `stdlib_net.hpp:524` | empty-catch | **Intentional** — best-effort `Content-Encoding` decode; on failure the raw body is kept (comment says so). Not a bug. |
| `regex_engine.hpp:561` | switch-missing-default | Internal `assertHolds(kind,…)`; `kind` is always a valid anchor op from the compiler. Defensive nit only (unreachable). |
| `stdlib_json.hpp:320` | implicit-widening `(depth+1)*indent` | **Minor hardening** — `int*int` widened to size_t; overflows only for a pathological huge `indent` (depth is guarded ≤1000). A huge indent OOMs regardless; capping `indent` would harden it. Low severity — candidate. |
| `hashing.hpp` (×12), other widenings | implicit-widening | Hash mixing / bounded sizes — intentional int arithmetic; no reachable overflow. |
| `runtime.hpp:618` | narrowing size_t→ptrdiff | Slice index math; bounded by container size. Fine. |

## Perf/style candidates (for the perf-variance pass — verify each is safe first)

- `performance-unnecessary-value-param` (21) — some are the deliberate native-binding lambda idiom (by-value `vm`/`self`); the rest may be genuine large-object-by-value.
- `performance-for-range-copy` (19) — range-for copying elements; `const auto&` where safe.
- `performance-enum-size` (14), `inefficient-vector-operation` (8), `inefficient-string-concatenation` (5) — micro-wins.

These do NOT change behaviour; they are throughput/variance candidates only, each needs a per-site
safety check (the native-binding by-value shadowing is intentional and must stay).

**Conclusion:** the codebase is static-analysis-clean for correctness. The v1.14 yield will come from the
per-subsystem adversarial agents (behavioural bugs static analysis can't see), the C++/`.ki` coverage
parity, DRY, and the perf pass.
