# Kirito v1.12 — Optimization & Hardening Audit

**Goal:** deep, comprehensive audit of the entire codebase for bugs, semantic/logical
correctness, and weak spots; harden them; guarantee every public method/attribute/class/
argument/feature is tested (C++ **and** `.ki`) from every angle (adversarial, fuzz, edge,
typical, malformed); enforce single-source-of-truth (DRY) for anything reused; and land
**low-risk, high-reward** optimizations that reduce Kirito's performance variance (the perf
sweep shows acceptable mean but high standard deviation).

This directory is the **durable record** of the effort. Every agent writes its own
`agents/<id>.md` continuously so nothing is lost if a worker is force-stopped. Commit
early and often.

## Probe binaries (already built — do NOT rebuild)
- `release/ki-linux-x64`  — fast Release+TLS `ki` for behavioural probes.
- `dist/debug-asan`       — ASan/UBSan `ki`: run adversarial `.ki` through it to catch memory/UB bugs.
- `dist/debug-tsan`       — TSan `ki`: for concurrency (`parallel`) probes.
- `.deps/build-ki-linux-x64/ki` — plain debug.

Write throwaway probe scripts to the scratchpad, NOT the repo.

## Method
1. **Audit (read-only).** Each agent statically + adversarially audits its area, runs probes
   against the binaries above, and records findings in its `.md`. Agents do **not** edit
   source/tests in this phase (avoids conflicts).
2. **Triage.** Main session collates all findings into `FINDINGS.md`, ranked by severity,
   de-duplicated, each marked confirmed/plausible.
3. **Harden + test.** Fix confirmed bugs and fill test gaps (C++ + `.ki`), each with a
   regression test. Verify via `post_work_check.sh` (debug→release→asan→tsan) + TLS gate.
4. **Optimize.** Address perf variance with low-risk changes; measure before/after.

## Finding format (in each agent .md)
```
### <AREA>-<n>  <one-line title>
- **Severity:** Critical | High | Medium | Low | Nit
- **Kind:** correctness-bug | crash/UB | resource-exhaustion | DRY | missing-guard | test-gap | perf | doc
- **Location:** src/kirito/<file>.hpp:<line>
- **What:** <precise description>
- **Repro / failure scenario:** <inputs -> wrong/crashing behaviour; a .ki snippet if possible>
- **Confidence:** confirmed (reproduced) | plausible (reasoned)
- **Fix sketch:** <how to harden>
```

## Agent roster & status

Legend: ⬜ not started · 🟡 running · ✅ done · ❌ stopped/incomplete

### Wave 1 — core engine (correctness-critical)
| ID  | Area | Files | Status |
|-----|------|-------|--------|
| A01 | Lexer | lexer.hpp | ⬜ |
| A02 | Parser + AST | parser.hpp, ast.hpp | ⬜ |
| A03 | Compiler / resolver / analyzer | compiler.hpp, bytecode.hpp, resolver.hpp, locals.hpp, analyzer.hpp | ⬜ |
| A04 | Bytecode VM | bytecode_vm.hpp, control.hpp, function.hpp | ⬜ |
| A05 | Runtime — operators/calls/members | runtime.hpp (dispatch, call protocol, member access, slicing, numeric fast path) | ⬜ |
| A06 | Runtime — built-in type methods | runtime.hpp (String/List/Set/Dict/Bytes/Integer/Float methods) | ⬜ |
| A07 | Value / object / class model | value.hpp, object.hpp, class_value.hpp, environment.hpp, module.hpp, native.hpp | ⬜ |
| A08 | Arena / GC / pool / hashing | arena.hpp, pool.hpp, handle.hpp, hashing.hpp | ⬜ |
| A09 | Collections / bytes / builtins / vm | collections.hpp, bytes.hpp, builtins.hpp, vm.hpp, common.hpp, exceptions.hpp | ⬜ |

### Wave 2 — standard library
| ID  | Area | Files | Status |
|-----|------|-------|--------|
| A10 | io / path / proc / sys | stdlib_io, stdlib_path, proc_compat, stdlib_sys, cli_paths | ⬜ |
| A11 | math / random | stdlib_math, stdlib_random | ⬜ |
| A12 | tensor + autograd | stdlib_tensor, tensor.hpp | ⬜ |
| A13 | matrix / complex | stdlib_matrix, stdlib_complex | ⬜ |
| A14 | net | stdlib_net, net_compat | ⬜ |
| A15 | serde (json/serialize/dump) | stdlib_json, stdlib_serde, stdlib_serialize, stdlib_dump | ⬜ |
| A16 | compression / hash | stdlib_zlib, stdlib_gzip, deflate, stdlib_hash | ⬜ |
| A17 | time | stdlib_time | ⬜ |
| A18 | regex | regex_engine, stdlib_regex | ⬜ |
| A19 | parallel / dispatcher | stdlib_parallel, dispatcher | ⬜ |
| A20 | ki-authored modules | stdlib_kimodules (itertools/functools/collections/statistics/string/textwrap/base64/csv/tabular/xml/heapq/bisect/copy/enum/tee/arg/semver) | ⬜ |

### Wave 3 — cross-cutting
| ID  | Area | Output | Status |
|-----|------|--------|--------|
| A21 | Test-coverage gap map (C++ + .ki) | every public symbol → its tests; list untested surface | ⬜ |
| A22 | Performance-variance analysis | sources of high stddev; low-risk optimization proposals | ⬜ |

## Status board (main session updates)
- 2026-07-04: workspace created; branch `claude-branch` restarted from main (ecd6a3f). kVersion still 1.11.0.
- 2026-07-04: **Wave 1 launched** — A01–A09 core-engine auditors running (read-only, each → its agents/Axx.md).
  Main session commits the .md files on each completion so findings survive a force-stop / container loss.
  Wave 2 (A10–A20) launches after Wave 1 reports; Wave 3 (A21–A22) after that.
