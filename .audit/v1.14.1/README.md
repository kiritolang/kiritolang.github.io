# Audit round v1.14.1 (Label Z)

A full-codebase deep audit on the hardened 1.14.0 base (crypto + arbitrary-precision int + secure
random just landed on `claude-branch`, unmerged). An audit loop is a **patch** release, so the target
version is **1.14.1**.

## Goal

Scan the ENTIRE codebase for bugs, semantic/logical incorrectness, and weak spots; harden with
adversarial / fuzz / edge-case tests (C++ **and** `.ki`). Keep single-source-of-truth (DRY) for
anything reused. Assure each `KiritoVM` is fully self-contained (no mutable global state). Verify the
docs page-by-page against the implementation; add anything missing. Every method / attribute / class /
argument / feature must be tested from every angle. Only fix REAL, triggerable errors; keep the code
idiomatic; never break a documented contract. Every fix gets a pinned regression test + a doc update.

Loop: if a round finds nothing of note, stop; else run another round.

## Durability protocol

Each scanner agent owns ONE `.md` file under `scan/` and appends findings to it AS IT GOES, so a
force-stop never loses work — a resumed agent (or the triager) picks up from the file. Findings are
`AXX-N` (agent + number). Scanners are READ-ONLY on the codebase (they find + propose; they do not
edit source/tests). Fixes happen centrally after triage, each with a regression test.

## Respect the false-positives table

`.audit/README.md` has a table of behaviours flagged in earlier rounds that are **correct by design**
(NaN write-only keys, unordered Dict/Set, left-only tensor scalar ops, `not <instance>` raw result,
Complex unhashable, BytesIO.seek clamps, semver leading-zero leniency, complex.polar negative rho,
tabular ragged/index, math.trunc→Integer, io.write stringifies Bytes, sys.exit skips fstreams, …).
Do NOT re-flag these; if you think one is wrong, argue it explicitly.

## Scanner assignments (each writes scan/AXX.md)

| Agent | Subsystem | Primary files |
|-------|-----------|---------------|
| A01 | Lexer, string/f-string literals, numeric literals | lexer.hpp |
| A02 | Parser + AST (all grammar, indentation, packing/unpacking) | parser.hpp, ast.hpp |
| A03 | Resolver, analyzer (static warnings), free-var/locals analysis | resolver.hpp, analyzer.hpp, locals.hpp |
| A04 | Compiler + bytecode + stack VM (control flow, slots, try/with/finally) | compiler.hpp, bytecode.hpp, bytecode_vm.hpp |
| A05 | Runtime: operators, numeric semantics, call protocol, compare, hash, member ops | runtime.hpp |
| A06 | Object model, arena, GC, small-object pool, vm.hpp; **KiritoVM self-containment** | object.hpp, arena.hpp, pool.hpp, vm.hpp, environment/function/module headers |
| A07 | Builtin scalar types + String/Bytes methods + format mini-spec | builtins.hpp, bytes.hpp |
| A08 | Collections List/Set/Dict/Array, iteration, apply, ordering, hashing | collections.hpp |
| A09 | C++ embedding API: value.hpp wrappers, native.hpp, Args, PinnedHandle/RootScope | value.hpp, native.hpp |
| A10 | io, path, sys, time + process/platform compat | stdlib_io.hpp, stdlib_path.hpp, stdlib_sys.hpp, stdlib_time.hpp, proc_compat.hpp |
| A11 | math, random (+secure), matrix, complex, tensor, hashing core | stdlib_math.hpp, stdlib_random.hpp, rand_compat.hpp, stdlib_matrix.hpp, stdlib_complex.hpp, stdlib_tensor.hpp, tensor.hpp, hashing.hpp |
| A12 | json, serialize, dump, serde core, zlib, gzip, hash module, crypto, int/BigInt | stdlib_json.hpp, stdlib_serialize.hpp, stdlib_dump.hpp, stdlib_serde.hpp, deflate.hpp, stdlib_zlib.hpp, stdlib_gzip.hpp, stdlib_hash.hpp, stdlib_crypto.hpp, stdlib_int.hpp |
| A13 | net (socket/HTTP/TLS), parallel, dispatcher, regex | stdlib_net.hpp, net_compat.hpp, stdlib_parallel.hpp, dispatcher.hpp, stdlib_regex.hpp, regex_engine.hpp |
| A14 | Kirito-authored frozen modules | stdlib_kimodules.hpp |
| A15 | **Docs vs implementation** — page by page | docs/pages/*.md vs code |
| A16 | **Test-coverage gap map** — what member/arg/path is untested (C++ + .ki) | tools/tests/** vs the full public surface |

Findings roll up into `FINDINGS.md` after triage.
