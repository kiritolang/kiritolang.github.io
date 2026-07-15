# Audit round v1.15 (target patch 1.15.1)

A full-codebase deep audit on the 1.15.0 base, which has just landed on `claude-branch` (unmerged):
- the **non-moving generational GC** (young/old, minor+major, write barrier + remembered set) — NEW
- **`Function`/`class` values serializable by default** (self-contained, source-capture + free-vars) — NEW
- strict lexical addressing (LoadGlobal/LoadVar/frame slots) — recent

An audit loop is a **patch** release, so the target version is **1.15.1**.

## Goal

Scan the ENTIRE codebase for bugs, semantic/logical incorrectness, and weak spots; harden with
adversarial / fuzz / edge-case tests (C++ **and** `.ki`). Keep single-source-of-truth (DRY) for
anything reused. Assure each `KiritoVM` is fully self-contained (no mutable global state). Every
method / attribute / class / argument / feature must be tested from every angle, like a C++ reference.
Only fix REAL, triggerable errors; keep the code idiomatic; never break a documented contract. Every
fix gets a pinned regression test (edge / fuzz / adversarial / bad-input) + a doc update.

Extra scrutiny this round on the two NEW subsystems (generational GC barrier completeness; function/
class serialization round-trips + boundary rules) — they are the highest-risk surface.

## Durability protocol (READ THIS)

Each scanner agent owns ONE `.md` file under `scan/` and appends findings to it **AS IT GOES**, so a
force-stop never loses work — a resumed agent (or the triager) picks up from the file. Findings are
`AXX-N` (agent number + finding number). Scanners are **READ-ONLY on the codebase** — they find and
propose (with concrete repro + proposed fix + proposed regression test), they do NOT edit source or
tests. Fixes happen centrally after triage, each with a regression test.

Each finding entry MUST contain:
- **ID** `AXX-N`, **severity** (HIGH memory-safety/corruption / MED wrong-result / LOW cosmetic),
  **confidence** (confirmed / likely / speculative)
- **Location** `file:line`
- **What** — the bug / weak spot / missing coverage, precisely
- **Repro** — a concrete `.ki` snippet or C++ sequence that triggers it (or "coverage gap: no test for X")
- **Proposed fix** — idiomatic, minimal, contract-preserving
- **Proposed test** — the adversarial/edge/fuzz case that pins it

## Respect the false-positives table

`.audit/README.md` has a table of behaviours flagged in earlier rounds that are **correct by design**
(NaN write-only keys, unordered Dict/Set, left-only tensor scalar ops, `not <instance>` raw result,
Complex unhashable, BytesIO.seek clamps, semver leading-zero leniency, complex.polar negative rho,
math.trunc→Integer, io.write stringifies Bytes, sys.exit skips fstreams, …). Do NOT re-flag these.
If you believe a rejection is wrong, argue it explicitly rather than silently proposing a revert.

## Agents

| Agent | Subsystem | Files |
|-------|-----------|-------|
| A01 | Lexer, string literals, f-strings, indentation, line-endings | lexer.hpp |
| A02 | Parser, AST, packing/unpacking, precedence | parser.hpp, ast.hpp |
| A03 | Compiler, bytecode, resolver, analyzer, locals, lexical addressing | compiler.hpp, bytecode.hpp, resolver.hpp, analyzer.hpp, locals.hpp |
| A04 | Bytecode VM, control flow, call protocol, exceptions dispatch | bytecode_vm.hpp, control.hpp |
| A05 | **Generational GC**, arena, object, pool, environment, handles (NEW — deep) | arena.hpp, object.hpp, vm.hpp, pool.hpp, environment.hpp, handle.hpp |
| A06 | Collections — List/Dict/Set + runtime methods | collections.hpp, runtime.hpp (collection methods) |
| A07 | Strings + Bytes + encoding + unicode + format | runtime.hpp (string methods), bytes.hpp, common.hpp |
| A08 | Numerics — Integer/Float/overflow/division, math, int/BigInt | runtime.hpp (numeric), stdlib_math.hpp, stdlib_int.hpp |
| A09 | Classes, functions, closures, super, exceptions, with, dunders | class_value.hpp, function.hpp, exceptions.hpp |
| A10 | **Serialization** — serde/serialize/dump + fn/class serialization (NEW — deep) | stdlib_serde.hpp, stdlib_serialize.hpp, stdlib_dump.hpp |
| A11 | complex, matrix, tensor + autograd | stdlib_complex.hpp, stdlib_matrix.hpp, tensor.hpp, stdlib_tensor.hpp |
| A12 | net/socket/TLS, proc, sys, time | stdlib_net.hpp, net_compat.hpp, stdlib_sys.hpp, proc_compat.hpp, stdlib_time.hpp |
| A13 | io, path, hash, crypto, zlib, gzip, random, json | stdlib_io.hpp, stdlib_path.hpp, stdlib_hash.hpp, stdlib_crypto.hpp, stdlib_zlib.hpp, stdlib_gzip.hpp, stdlib_random.hpp, stdlib_json.hpp, rand_compat.hpp, hashing.hpp |
| A14 | regex engine + module | regex_engine.hpp, stdlib_regex.hpp |
| A15 | C++ embedding API | value.hpp, native.hpp |
| A16 | Frozen `.ki` stdlib modules | stdlib_kimodules.hpp |
| A17 | parallel + dispatcher (concurrency, share-nothing, deadlock-safety) | dispatcher.hpp, stdlib_parallel.hpp |
| A18 | Cross-cutting: test-coverage-gap map + perf-variance analysis | (all tests + benchmarks) |

## Output

Each agent's raw findings live in `scan/AXX_<name>.md`. After all agents report, the triager
consolidates into `FINDINGS.md` (ranked, deduped, with verdicts), then fixes land centrally with
regression tests, validated on debug + release.
