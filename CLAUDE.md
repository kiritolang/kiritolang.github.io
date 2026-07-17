# CLAUDE.md — Kirito engineering guide

**Read this in full at the start of every session.** It is the source of truth for *how we work* and
the *shape of the system*. The exhaustive language/stdlib/API reference lives in **`docs/`** (authored
Markdown in `docs/pages/`, rendered by `docs/build_docs.py`) — this file does **not** duplicate it.

**Kirito** is a from-scratch, dynamically- but strongly-typed, general-purpose scripting language
(`.ki` files; namespace `Kirito`), implemented as a **bytecode compiler + stack VM** in header-only
**C++20**. It is both a standalone interpreter (`ki`) and an embeddable library (Lua-style: one
`KiritoVM` = one fully-encapsulated process). Design goal: a high-level, low-boilerplate language that
is also an easy-to-extend C++ framework.

---

## Repository structure

- **`src/`** — the interpreter (header-only `src/kirito/*.hpp` behind the umbrella `src/kirito.hpp`) and
  `main.cpp` (the `ki` entry point). `src/fum/` is a vendored hash map.
- **`tests/`** — everything test-related: `unit/*.cpp` (CTest), golden `scripts/*.ki` + `.expected`,
  the error suite `errors/*.ki` + `.experr` (a program that must fail with required diagnostic text),
  `fuzz/`, and `CMakeLists.txt` (auto-discovers tests via `CONFIGURE_DEPENDS` globs).
- **`docs/`** — documentation, references, the course, and the doc site builder.
- **`tools/`** — hardened scripts (`tools/scripts/`) and the pinned-environment record
  (`tools/versions.env`). `kpm/` (the package manager, written in Kirito) is at the repo root.
- **`examples/`** — single-file demos and `examples/big_projects/` (larger multi-file programs that
  double as stress tests).
- **`.audit/`** — hidden but tracked; **frozen records of past audits** (`v1.NN/…`). Never rewrite a
  historical audit record; a new audit round gets a new directory.
- **`license/`** — `LICENSE` (MIT) + `THIRD_PARTY_LICENSES.md`. Never add/remove/modify a license here
  without explicit permission.

**Script threshold.** Any routine (setup, build, run, test) that takes more than one command, or a
single command longer than ~60 characters, must be a **hardened script in `tools/`** — not run ad hoc.
Hardened means: validate its own preconditions and arguments, detect a misconfigured environment or
missing tools, and fail with a clear, actionable error rather than crashing opaquely or proceeding on
bad assumptions. **Self-contained:** any required config / data download / install step must likewise be
a hardened `tools/` script, never an undocumented manual step.

**Pinned environment.** `tools/versions.env` records the exact toolchain the project builds against;
`tools/scripts/check_env.sh` validates the local environment against it (used by contributors/CI).
Keep both current. Kirito has **no third-party runtime dependencies** (OpenSSL is optional, only for the
TLS/crypto features), so the reproducibility surface is just compiler + CMake + Ninja (+ optional OpenSSL).

---

## Workflow

- **Start clean.** Confirm you are on the correct branch and have pulled remote changes before starting.
- **Audit → plan → implement.** Read/run/test the relevant code before making any claim about existing
  behavior — never assert from memory or assumption. Understand the current state, then plan, then implement.
- **Never assume a request is correct as stated.** Take the intent seriously, but analyze the requested
  change against the actual codebase and proactively flag risks, consequences, or better alternatives —
  correct a wrong assumption rather than silently complying or silently working around it.
- **Audit system-wide impact** before and during a change — callers, shared state, invariants relied on
  elsewhere — against the real sources, not memory. Revisit if the scope grows.
- **Keep docs, references, and tests up to date in the same change**, never as a follow-up.
- **Test before pushing:** run the scoped tests covering the change. Before opening a PR: the full suite
  (the 4-variant gate below).

### Git — branch `claude-branch` only

**Only commit and push to `claude-branch`.** Never commit/push to `main`, never force-push `main`. Open a
pull request only when explicitly asked; expect the branch to be deleted after merge (recreate freely from
`origin/main` for new work — rebase only if `main` advanced with conflicting changes).

```sh
git fetch origin main
git checkout -B claude-branch origin/main   # branch missing, or its previous PR already merged
git checkout claude-branch                  # branch exists with unmerged work — keep going (never -B over it)
```

Push whenever a logically-complete change is done and its scoped tests pass — don't leave completed work
unpushed. A PreToolUse hook (`.claude/hooks/enforce_claude_branch.py`) blocks commits off `claude-branch`
and pushes touching `main`; if it fires, switch to `claude-branch` and retry — do not bypass it.

### Versioning

`kVersion` in `src/kirito/version.hpp` (surfaced as `ki --version` / `sys.version`) is the version the
**next** release *will* carry — editing it neither releases nor authorizes a tag/GitHub Release. "Bump to
X" means only "edit `kVersion` (+ docs that quote it)". A release happens **only** when the user explicitly
asks (build binaries, push tag, upload Release). An **audit/hardening round is a PATCH bump**
(`1.12.0`→`1.12.1`); a **minor** bump is for genuinely new user-facing features, a **major** for breaking
changes — and only when asked. (`.audit/v1.NN` are round labels, not versions.)

---

## Architecture (as built)

- **Header-only core.** The whole interpreter is `src/kirito/*.hpp`, surfaced through `#include "kirito.hpp"`
  (embeds Kirito, no `main`). Use `#ifndef` guards (never `#pragma once`); everything `inline`/templated;
  **no mutable globals** — all state is VM-scoped.
- **One `KiritoVM` = one encapsulated process**, owning an `ObjectArena`, the global `Environment`, and the
  `ModuleRegistry`. No global/static mutable state → multiple VMs coexist and a VM's whole context is
  serializable. The arena is unsynchronized, so **exactly one OS thread may touch a given VM**; concurrency
  is therefore **multiprocessing** — a `KiritoDispatcher` owns the main VM + one worker VM per thread + the
  cross-VM primitives, and the `parallel` module exposes it. Workers share nothing; they exchange only
  serialized blobs through thread-safe queues/primitives.
- **Pipeline (keep the stages separate, each behind a clean interface):**
  `.ki → Lexer → [tokens] → Parser → [AST] → Compiler → [bytecode Proto] → BytecodeVM → result`.
  The **AST is the stable boundary**: the `Compiler` (`compiler.hpp`) is a second AST visitor (alongside the
  parser) that lowers each body to a flat `Proto` (`bytecode.hpp`); the `BytecodeVM` (`bytecode_vm.hpp`)
  executes it with an explicit GC-rooted operand stack instead of native recursion. Operator/call/member
  semantics are shared free functions in `runtime.hpp`. A compile-time, scope-aware **resolver** turns every
  name into an O(1) slot/global index (strict lexical addressing; read-before-write is a `name not defined`
  error, not an outer-scope fallback).
- **VM-owned value graph + handles.** Every value is an `Object` in an arena slot (`unique_ptr`); everything
  else holds lightweight `Handle`s (slot+generation). Reference-assignment = two bindings sharing one handle.
  No `shared_ptr`, no per-value refcount. Reclamation is a **precise, non-moving, generational mark-sweep GC**
  (young nursery + old gen; write barrier + remembered set for old→young edges; **card marking** so a
  container grown in a loop is O(N) not O(N²); minors promote in place, majors reclaim old cycles). A
  `Handle` stays valid across any collection (non-moving) and its generation catches use-after-free. **GC
  rule for native code:** root a value the moment it is made (`RootScope`/`PinnedHandle`), not once its owner
  is arena-reachable — an off-arena container is traced by nothing. See `docs/` and the `GC` memories for the
  subtle invariant that an object buffering young handles in non-arena storage must be re-traced from its root.
- **Unified object protocol.** Built-ins, C++-authored `NativeClass`es, and Kirito `class`es all derive from
  one `Object` exposing the same slots (`truthy/str/equals/hash` + `binary/unary/call/getAttr/setAttr/
  getItem/setItem/iterate/lazyIterate/length`). The VM cannot tell them apart.
- **Extending in C++.** Prefer the ergonomic `value.hpp` wrappers (`Value`/`Integer`/`Float`/`String`/
  `Bytes`/`List`/`Set`/`Dict`) — they mirror Kirito's operators/methods and barrier/root correctly. For new
  behavior, subclass `NativeModule` (override `setup`) or `NativeClass` (override only the slots you need) and
  register with one call — indistinguishable from a built-in to the VM. A host storing a value in a long-lived
  C++ object holds a `PinnedHandle`, never a bare `Handle`.

**Status:** broadly implemented and tested end-to-end. The bytecode VM is the sole engine (no tree-walker).
Full feature/stdlib/API details are in `docs/pages/` — do not re-enumerate them here.

---

## Build & test

Toolchain: see `tools/versions.env` (g++ 13, clang++ 18, cmake 3.28, ninja; C++20). Run
`tools/scripts/check_env.sh` to verify. Thin, out-of-source CMake: the header-only core is an `INTERFACE`
target; CMake builds `ki` (from `main.cpp`) + the test executables, reusing a shared precompiled header.
Static linking by default (self-contained binaries). Cross-platform (Linux + Windows minimum); the only
platform-specific code is behind `net_compat.hpp` / `proc_compat.hpp` / `rand_compat.hpp`.

Four CMake presets, each a full clean build of the auto-discovered CTest suite:
**`debug`** (g++ `-O0`, the strictest warning gate: `-Werror -Wall -Wextra -Wconversion -Wshadow …`),
**`release`** (`-O2`, ship/benchmark), **`asan`** (Address/UBSan), **`tsan`** (ThreadSanitizer — the
data-race gate for the `parallel` dispatcher, the only concurrent code).

**Post-work gate** (`tools/scripts/post_work_check.sh`, contract in `.claude/POST_WORK_CHECKLIST.md`): runs
the variants **sequentially** — `debug`, then `release`, **commit+push once both are green**, then `asan`
and `tsan` (fix and re-push any failure). Run it before calling a change done.

> **Never run two builds at once, and never hand-roll `-j$(nproc)`.** Sequential is a **memory** constraint,
> not style: every test TU includes the whole header-only interpreter, so one compile peaks ~1.7 GB RSS at
> `-O2` (~3.2 GB under asan), and peak build RAM = `jobs × that`, scaling with CORE COUNT. On the WSL2 dev box
> (24 cores / 47 GB) two variants at once OOM the kernel and **reboot the distro**. Let `post_work_check.sh`
> size `-j` (it caps jobs from MemAvailable and refuses to start beside another build); never background a
> second variant.

Other checks (all take `--ki PATH`): release-binary smoke tests (`test_examples.sh`, `test_big_projects.sh`),
doc-as-test (`test_docs_examples.py` runs every ```kirito fence in the docs), and the golden round-trip
(`bytecode_stability.sh`). Opt-in coverage-guided fuzzer (`-DKIRITO_ENABLE_LIBFUZZER=ON`, needs clang).
Nightly CI (`.github/workflows/`) runs the gate + cross-compile + smoke tests. **Before claiming something
works, actually build and run it; report real output.**

---

## General programming rules

- **Single Source of Truth (SSOT / DRY).** Every behavior has exactly one authoritative, isolable
  implementation. Reuse via parameterization/composition/generics; extend the existing one rather than
  forking a parallel copy. Eliminate duplicated *knowledge*, not just duplicated code.
- **Self-explaining code.** Expressive names over comments. Comments explain *why* (assumptions, invariants,
  complexity, non-obvious tradeoffs), never *what* the code already says. If a comment is needed to say what
  code does, improve the code.
- **Never allow silent failures.** Expose failure through the idiomatic mechanism so ignoring it is hard.
  Fail fast when an invariant is violated. A value that can't be valid should throw on use, not propagate.
- **No silent fallbacks or workarounds in our own code** — no papering over a bug with a fallback / retry-and-
  hope / silent degradation; fix the cause. (This does *not* apply to genuinely **external** failure modes —
  a flaky service, transient network — where an explicit, documented fallback is legitimate.)
- **Structured, diagnostic errors.** Errors carry **what** (a specific condition), **where** (operation/site —
  Kirito errors carry file:line:col + a traceback), and context (expected vs actual, offending input). Not
  bare, generic strings.
- **KISS + scope generality to the module, not the feature.** Prefer the simplest approach that meets the
  required complexity bounds. Build the *complete* version of the thing you're building (that class/algorithm),
  but do not add speculative features nobody asked for (YAGNI). Design for later extension without pre-building
  it.
- **Deterministic ownership (RAII).** One owner per resource, released automatically (destructor/`with`/scope
  guard). No leaks. Never expose raw owning pointers — favor references, else `std::unique_ptr` over
  `shared_ptr`.
- **Preserve invariants; make invalid states unconstructible.** Types stay valid throughout their lifetime.
- **No implicit/unsafe conversions.** No silent narrowing/truncation/coercion that loses data or changes
  meaning; a lossy/failing conversion must be explicit and checked. (The codebase is `-Wconversion`-clean.)
- **No undefined behavior.** Never rely on or trigger UB (OOB, use-after-free, signed overflow, data races,
  aliasing). Guarantee it with sanitizers/safe abstractions, not hope. (asan/ubsan/tsan gate the build.)
- **No global mutable state / singletons.** State is owned explicitly (an object, a module, or an injected
  dependency) and its mutation is traceable to one owner and scope. All Kirito state is VM-scoped.
- **Separate concerns.** Keep logic independent of I/O/net/OS/UI behind clean interfaces; depend on interfaces,
  not concretions (see the `*_compat.hpp` isolation layer).
- **Prefer pure functions; single-purpose functions.** Return values over mutable out-params; don't mutate
  state you don't own. Each function does one conceptual thing (extract helpers rather than bundling several).
- **Small, hard-to-misuse public APIs.** Hide implementation details; make invalid use difficult and correct
  use natural. Validate inputs and enforce contracts at module boundaries (internal code may assume them).
- **Prefer immutability/const-correctness, determinism, and the standard library** over mutation, nondeterminism,
  and third-party deps (unless an external dep gives substantial justified value; randomness/time/external
  systems are deterministic-exempt only when explicitly part of the contract).
- **Consider performance in design, never at the cost of correctness or an algorithm's big-O.** Optimize only
  measured bottlenecks. **Document non-trivial complexity, synchronization, and invariants.**

### Naming: Kirito's public surface is lowercase, no underscores

Every Kirito-visible name (builtins, stdlib functions, type methods) is all-lowercase, no underscores/camelCase
(`gettempdir`, `startswith`, `symmetricdifference`, `timens`). C++ identifiers use ordinary C++ style.

---

## Testing rules

- **Every non-trivial piece of code MUST be thoroughly tested**; prefer many small, single-behavior tests.
- Unit-test components in isolation. Test **edge/boundary/invalid/empty inputs and adversarial attempts** to
  break the code. Test **failure paths**: exceptions, cleanup, rollback, resource release under failure.
- Use **property-based / fuzz** testing where applicable; **randomized** testing with deterministic **seeds**
  when reproducibility matters, and sanity-check the outputs.
- Verify **critical algorithms** against independent implementations, mathematical properties, or sanity checks.
- **Every discovered bug ships a regression test** (before or with the fix), named for the *symptom* it covers
  (`spec_dict_iter_after_delete.ki`, not `fix_pr47`), so it can never silently rot. A feature isn't done until
  it has a test and the suite is green.
- Design for testability: abstract db/fs/clock/rng/net behind interfaces or inject them.

---

## General philosophy

Correctness before performance. Simplicity before cleverness. Readability before brevity. Reusability through
good abstractions, not speculative features. Every abstraction must reduce overall complexity — if it doesn't,
remove it. Every module should be understandable, testable, and replaceable in isolation, and make invalid
usage difficult and correct usage natural.

---

## Keep this file current

When a decision changes the language design, architecture, or workflow, update this file **in the same change**.
It must always describe Kirito as it actually is. Put exhaustive feature/API detail in `docs/`, not here.
