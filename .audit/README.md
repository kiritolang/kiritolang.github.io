# Kirito audit archive

The durable, git-tracked record of every deep-audit round run against the Kirito codebase. Each round
is a full-codebase sweep — parallel per-subsystem agents + static analysis produce findings, which are
triaged, verified, then fixed with regression tests (C++ **and** `.ki`) and validated across the
`debug` / `asan` / `tsan` build presets.

This directory is intentionally **hidden** (`.audit`) but **tracked** — it is history/scaffolding, not
part of the shipped product, yet must never be lost. The canonical, always-current description of the
language and its architecture lives in `CLAUDE.md` and `docs/`, not here; this archive is the paper
trail of *how* the code got hardened.

## Rounds (chronological)

| Round | Directory | Scope | Entry point |
|-------|-----------|-------|-------------|
| **pre-1.12** | [`pre-1.12/`](pre-1.12/) | First full-codebase audit — static analysis (clang `--analyze`) + subsystem agents, kept as one running log. | [`AUDIT_NOTES.md`](pre-1.12/AUDIT_NOTES.md) |
| **v1.12** | [`v1.12/`](v1.12/) | Optimization & hardening: core-engine + stdlib agents (A01–A22), coverage-gap map, perf-variance analysis. | [`README.md`](v1.12/README.md) · [`FINDINGS.md`](v1.12/FINDINGS.md) · [`agents/`](v1.12/agents/) |
| **v1.13** | [`v1.13/`](v1.13/) | The most exhaustive round — every builtin/stdlib/type/operator/dunder probed from every angle (A01–A26), DRY consolidation, GC-variance perf work. | [`README.md`](v1.13/README.md) · [`FINDINGS.md`](v1.13/FINDINGS.md) · [`agents/`](v1.13/agents/) |

Each round's `FINDINGS.md` (or the pre-1.12 `AUDIT_NOTES.md`) is the triaged roll-up; the `agents/*.md`
are the raw per-subsystem findings that feed it. A finding is only "done" once it is either fixed with
a pinned regression test or explicitly rejected with a reason.

## Layout of a round

```
<round>/
  README.md      plan, scope, durability protocol           (v1.12+)
  FINDINGS.md    triaged, ranked roll-up: fixed / test-added / todo / wontfix
  agents/AXX.md  one file per subsystem agent — raw findings, kept verbatim
```
(The pre-1.12 round predates that split and is a single `AUDIT_NOTES.md` log.)

## Reading order for a new session

1. This file — which rounds exist and what each covered.
2. The **latest** round's `FINDINGS.md` — the current triaged state and what is still open.
3. That round's `agents/*.md` for the raw detail behind any finding.
4. Earlier rounds only for provenance — the fixes they describe are already in the code and in
   `CLAUDE.md` / `docs/`; do not re-apply them.

## Continuity note

Findings are numbered per round (`AXX-N`), so an `AXX` id is only unambiguous **within its round** —
v1.12 `A18` (net) and v1.13 `A18` (net) are different documents. Always qualify by round when citing.
Later rounds re-audit earlier subsystems from scratch rather than diffing, so a genuinely fixed bug
should not reappear; if it does, that is itself a finding (a regression the earlier round's test missed).
