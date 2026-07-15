# A03 Compiler/Resolver

Round v1.15 (target patch 1.15.1). Scope: `src/kirito/compiler.hpp`, `src/kirito/bytecode.hpp`,
`src/kirito/resolver.hpp`, `src/kirito/analyzer.hpp`, `src/kirito/locals.hpp`. Read-only audit +
empirical checks against `./build-debug/ki`.

Status: IN PROGRESS.

Context: this round's headline new subsystem touching my files is the **function/class
self-serialization** feature (commit `9f0674a`), which added `freeVariables`/`eagerFreeVariables` to
`locals.hpp` (the dual of the existing `capturedLocals`). Prior rounds (v1.13, v1.14, v1.14.1) already
gave the resolver/analyzer/locals slot-layout machinery a clean bill of health (see those rounds'
`agents/A03_*.md` for what was already checked and found correct — not re-litigated here unless a
regression is found). This round's scan concentrates extra scrutiny on the new free-variable scan,
plus a fresh adversarial pass (nested closures, float const dedup, switch, self-shadowing) per the
brief.

