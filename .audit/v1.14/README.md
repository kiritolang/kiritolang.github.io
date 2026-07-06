# Kirito v1.14 — deep audit + C++/`.ki` test-coverage parity + perf-variance

**Goal.** A fresh, exhaustive audit of the whole codebase for bugs, semantic/logical correctness, and
weak spots; harden them; enforce single-source-of-truth (DRY); guarantee **every** class / method /
attribute / argument / feature is tested from every angle (typical, edge, adversarial, malformed,
fuzz/random) in **both C++ and `.ki`**; and land low-risk/high-reward perf-variance wins.

This round builds on v1.12/v1.13 (see `../v1.12/`, `../v1.13/`) which already scanned every subsystem
and exhaustively `.ki`-tested every documented symbol (the `verify_*.ki` corpus, ~3200 assertions).
So v1.14 targets the **new marginal yield**:

1. **Automated static analysis** — `clang-tidy` (bugprone/cppcoreguidelines/portability/performance)
   over the whole interpreter via `main.cpp`; triage real defects. Output under `static/`.
2. **C++ test-coverage parity** — the `.ki` verify pass was thorough; confirm the **C++ unit tests**
   reach the same corners (the embedding `Value`/`PinnedHandle`/`RootScope` API, native-class
   internals, GC/arena/pool internals, error-message exactness) and extend where they don't.
3. **Fresh adversarial bug-hunt** — re-scan each subsystem from a *different* angle than v1.13
   (interactions between modules, lifetime/ownership, integer overflow, resource guards, reentrancy).
4. **DRY / single-source-of-truth** — find remaining duplicated logic that could drift.
5. **Perf variance** — re-measure the release bench stddev and find another low-risk win.
6. **Re-verify** every v1.13 fix was a real bug (revert non-bugs); confirm no contract broke.

## Durability protocol (READ FIRST)

Each spawned agent gets **its own** `agents/<id>_<area>.md` and writes findings there **incrementally**
as it works — never batched at the end — so nothing is lost if a worker is force-stopped or usage runs
out. Agents are **READ-ONLY on `src/`** (they scan + record; the orchestrator fixes + tests). The
orchestrator commits+pushes after each agent lands so progress survives a container reclaim.

## Finding format (in each agent `.md`)

```
### AXX-N: <one-line>
- severity: high | medium | low | dry | coverage-gap | perf | non-bug
- location: src/kirito/<file>.hpp:<line> (+ docs/test refs)
- category: correctness | memory | overflow | resource | reentrancy | dry | coverage | perf
- description: <what & why it's wrong>
- failure-scenario: <concrete input/state -> wrong output/crash>
- proposed-test: <the .ki or C++ test that would pin it>
- proposed-fix: <if any; else "flag only">
- confidence: high | medium | low
```

## Subsystem map (one agent per line, `AXX` id)

- A01 lexer + parser + ast (tokenizing, indentation, string/f-string, error spans)
- A02 resolver + analyzer + locals (scope, capture, warnings)
- A03 compiler + bytecode (lowering, switch, finally/with cleanup, const dedup, slots)
- A04 bytecode_vm (operand stack, block stack, exceptions, jumps, GC roots)
- A05 runtime dispatch (operators, calls, member access, numeric fast path, kiLessThan/kiEquals)
- A06 object model (object/value/class_value/instance_value; dunders, privacy, _super_)
- A07 arena + gc + pool (handles, generations, mark-sweep, small-object pool, PinnedHandle)
- A08 collections (List/Set/Dict internals, hashing, probe reentrancy, ordering)
- A09 string + bytes (code points, encodings, methods, format mini-spec)
- A10 builtins (runtime.hpp builtin surface + constructors)
- A11 io + path (streams, files, BytesIO, filesystem)
- A12 math + random (domain errors, distributions, determinism)
- A13 tensor + matrix + complex (engine, autograd, linalg, broadcasting)
- A14 json + serde (serialize/dump, refs/cycles, class round-trip)
- A15 net + parallel + dispatcher (sockets, HTTP, worker VMs, concurrency)
- A16 sys + time + proc_compat (env, process, clocks, DateTime)
- A17 zlib + gzip + hash + regex (compression, digests, NFA engine)
- A18 ki-modules (itertools/functools/collections/heapq/bisect/copy/enum/statistics/string/textwrap/tee/arg/base64/csv/tabular/xml/semver)
- A19 embedding C++ API (value.hpp/native.hpp: Value wrappers, RootScope, PinnedHandle, NativeClass/Module) — **C++ test parity focus**
- A20 DRY / single-source-of-truth sweep (cross-cutting)
- A21 C++-vs-`.ki` coverage-gap map (which behaviours are tested in one but not the other)
- A22 perf-variance re-measurement (orchestrator-run)

## Triage → fix → test → validate

Findings roll up into `FINDINGS.md` (dedup, rank, confirm-real-before-fixing). Confirmed bugs are fixed
idiomatically with a pinned regression test (C++ **and/or** `.ki`), then validated debug → asan → tsan.
Non-bugs are recorded in `../README.md`'s false-positives list. Docs updated in the same change.
