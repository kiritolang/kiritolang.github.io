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
- 2026-07-04: **Wave 1 COMPLETE (9/9).** Tally: 6 High, 14 Medium, 17 Low, 2 Nit. No Critical; the GC core
  (mark-sweep, generation retirement, children() coverage) and control-flow lowering held up clean under ASan.

  **High findings (fix first):**
  - A04-1 — call-depth guard is count-based; recursion through a native HOF (`sorted(key=g)`, `xs.sort(key=g)`,
    `apply(g)`) overflows the native stack → **SIGSEGV** before the guard fires. Fix: stack-pointer-aware guard.
  - A07-1 — `InstanceValue::equals` `static_cast`s any `ValueKind::Instance`, but native types also report Instance
    → wrong-type downcast reading a garbage Handle (UBSan-confirmed), reachable as a Dict/Set key. Fix: `dynamic_cast`.
  - A08-1 — thread-local small-object pool leaks each worker's free-list at thread exit (~0.87 MB per `spawn`,
    unbounded, sanitizer-invisible). Fix: drain free-lists on thread exit.
  - A09-1 — `pow(base,exp,mod)` computes `((base%mod)+mod)%mod` in int64 which overflows before the `__int128`
    widen → silently wrong result in release + UB for mod > ~2^62.
  - A09-3, A09-4 — `Set`/`Dict` constructors and `zip`/`map`/`filter`/`sorted`/`enumerate`/`all`/`any` don't
    GC-root the handles from `iterate()` → dangling handles.

  **Cross-cutting THEMES (single fix each, high leverage):**
  - **GC-rooting of fresh-alloc / snapshot / iterate handles** — recurs in A06-1/2 (apply/sort), A07-4 (String iter),
    A09-3/4 (constructors + 7 builtins). A shared `rootedSnapshot` / rooted-iterate helper fixes them all at once.
  - **DRY: kwarg binding + param-default resolution duplicated 3–4×** with divergent policies — A05-2 (`makeMethod`
    None-fill → `d.setdefault(default=7)` inserts `{None:7}`), A07 (three binders), A03-3/4 (resolver rejects
    `Function(n, size=n)` the binder accepts). Unify into one binder / one resolution rule.
  - **Missing output-size resource guards** — A06-8 (`replace`/`join` OOM the host), unlike `*`/`ljust`/`center`.
  - **Unbounded retention in long-lived VM** — A08-2 (dispatcher Tasks), A09-2 (Dict/Set buckets never compact).

  Other confirmed Mediums: A01-1 (`\xHH` raw byte breaks String code-point layer), A02-1 (inline `return a,b` doesn't
  pack), A02-2 (f-string runtime errors report line 1), A07-3 (private-access asymmetry vs spec), A07-2/A05-1/A05-3
  (comparison/reflection asymmetries). Full detail + repros in agents/A01..A09.md.
- 2026-07-04 17:00 UTC: session usage limit hit mid-Wave-2 (all A10–A20 agents died at startup; only A10 wrote 1 finding).
  Limit reset at 17:00 UTC; **re-launched Wave 2 (A10–A20) + Wave 3 (A21 coverage-map, A22 perf-variance)** with
  compact prompts leaning on this README's themes. Wave-1 findings were all committed before the interruption.
