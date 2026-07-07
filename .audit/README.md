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

**Naming:** an audit loop is a **patch (bugfix) release**, so from `1.12.1` onward each round's directory
is its target patch version (`v1.12.1`, `v1.12.2`, …). The earlier `v1.12`/`v1.13`/`v1.14` labels predate
this convention — `v1.12` shipped as the `1.12.0` release; `v1.13`/`v1.14` were minor-style round labels
for work that remained unreleased and now folds into `1.12.1`.

| Round | Directory | Scope | Entry point |
|-------|-----------|-------|-------------|
| **pre-1.12** | [`pre-1.12/`](pre-1.12/) | First full-codebase audit — static analysis (clang `--analyze`) + subsystem agents, kept as one running log. | [`AUDIT_NOTES.md`](pre-1.12/AUDIT_NOTES.md) |
| **v1.12** | [`v1.12/`](v1.12/) | Optimization & hardening: core-engine + stdlib agents (A01–A22), coverage-gap map, perf-variance analysis. | [`README.md`](v1.12/README.md) · [`FINDINGS.md`](v1.12/FINDINGS.md) · [`agents/`](v1.12/agents/) |
| **v1.13** | [`v1.13/`](v1.13/) | The most exhaustive round — every builtin/stdlib/type/operator/dunder probed from every angle (A01–A26), DRY consolidation, GC-variance perf work. | [`README.md`](v1.13/README.md) · [`FINDINGS.md`](v1.13/FINDINGS.md) · [`agents/`](v1.13/agents/) |
| **v1.14** | [`v1.14/`](v1.14/) | Marginal-yield round on the hardened base — clang-tidy static analysis, fresh adversarial re-scan (A01–A19), C++/`.ki` coverage parity, perf-variance re-measure. 18 fixes (1 HIGH heap-corruption + 11 MED + 6 LOW); debug/asan/tsan green. | [`README.md`](v1.14/README.md) · [`FINDINGS.md`](v1.14/FINDINGS.md) · [`agents/`](v1.14/agents/) |
| **1.12.1** | [`v1.12.1/`](v1.12.1/) | First patch-versioned loop — 27 per-subsystem scanners (own `.md` each). Fixes: 3 HIGH memory-safety (Dict/Set stringify UAF, `tensor.split` OOB, deep-import segfault), 2 HIGH dunder (`_super_` corruption + privacy-bypass), 2 MED (value.hpp comparison UB, "unhashable" message drift), NaN display + islice guards; 1 reverted non-bug (median NaN). debug 797/797 + asan-clean. | [`README.md`](v1.12.1/README.md) · [`scan/`](v1.12.1/scan/) |

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

## False positives / rejected findings (do NOT re-fix)

Every round flags some behaviours that turn out to be **correct by design** or otherwise not bugs.
Because each round re-audits from scratch, the *same* false positive tends to resurface — so it is
recorded here once, with the reason, to stop it being re-litigated (or "fixed" into a regression).
Consult this list before acting on any finding that proposes changing one of these behaviours; if you
believe a rejection is wrong, argue it explicitly rather than silently reverting a load-bearing choice.

| Behaviour (auditor-flagged) | Verdict | Why it is not a bug |
|---|---|---|
| `math.trunc(x)` / `round(x)` return an **Integer**, not a Float (v1.12 A11-1) | by design | Kirito's Integer is int64 and truncation yields a whole number; returning Float would lose the type. Confirmed non-bug; an earlier "fix" was reverted. |
| A NaN used as a Set element / Dict key is **write-only** — you can insert it but never find it, and two NaNs both store (v1.12 A06-7) | by design | `==` on floats is exact IEEE-754, and `NaN != NaN`; membership/hashing agree with `==`. Load-bearing (`r7_types.ki`). *Note:* the v1.13 tabular NaN-key crashes (v1.13 A25-1/A25-2) were fixed at the **tabular** layer — drop NaN keys, pandas-style — precisely because the core NaN-key semantics are intentional and must not change. |
| Dict / Set iteration order is **not** insertion-stable across delete (pre-1.12) | by design | Kirito's Dict and Set are documented as **unordered**; no insertion-order guarantee is made. |
| `Semaphore.release` "over-release" and `Lock.release` by a non-owner are accepted, not rejected (pre-1.12) | intentional | Matches the permissive counting-semaphore / advisory-lock contract; rejecting would break valid producer/consumer patterns. |
| Scalar-on-the-**left** tensor arithmetic throws — `2 * t` fails, `t * 2` works (v1.13 A15) | documented limitation | Kirito has no reflected (`_radd_`-style) dunder, so `applyBinaryOp` never dispatches to the right operand. A documented invariant (`r11_docinvariants.ki`), not a bug — an ergonomics gap noted for a future reflected-op feature. |
| `io.write`/`io.print` stringify a `Bytes` argument (repr) instead of writing raw bytes; `BytesIO(Bytes(...))` is rejected (v1.13 A12-5) | intentional | Text vs binary streams are distinct; raw-byte output goes through a binary-mode `File`/`open(..., "wb")`. The `BytesIO(Bytes)` rejection is tested (`r4_io.ki`). |
| A03 analyzer "assigned-but-never-used" / "unused-result" flags on use-before-`var` or a shadowed call target (v1.13 A03-1/A03-3) | analyzer false positive | Non-fatal *warnings*, not exceptions; the flagged code runs correctly. Tightening the analyzer risks false negatives; left as-is. |
| `not <instance>` returns the **raw** `_not_` result, uncoerced to a Bool (v1.14 A04-1) | by design | Like `_neg_` (and unlike `_bool_`), the unary-operator dunder is not type-checked. Documented + tested (`r7_types.ki`: "returns the raw value"). A v1.14 "fix" that enforced Bool was reverted after it broke that test. |
| `!=` for a class defining only `_ne_` is symmetric on both sides (v1.14 A04-4, re-flagged as "still open") | non-bug (already fixed) | `c != 5` and `5 != c` both dispatch the reflected `_ne_` (runtime.hpp reflected-Eq/Ne branch). The v1.13 A06-1 fix is in place; the re-confirmation was mistaken. |
| `Complex` has value equality but is **unhashable** (can't key a Set/Dict) (v1.14 A13-4) | deliberate | `Complex(2,0) == 2` (cross-type real equality) means a consistent hash would have to match Integer/Float hashing exactly; a subtly-wrong hash corrupts Set/Dict (worse than unhashable). Unhashable is the safe choice — same stance as write-only NaN keys. |
| `sys.exit` flushes std streams + C stdio but **not** open user `fstream`s (v1.14 A19-5) | documented tradeoff | It uses `std::_Exit` for deadlock-safety from a `parallel` worker (matching Python's `os._exit`), which skips fstream destructors. Close files or use `with`. |

**Evolved case worth noting:** inline-function `return a, b` "doesn't pack" was **rejected as by-design**
in pre-1.12 and v1.12 (an inline body is a single-expression thunk; the comma belongs to the enclosing
context). v1.13 kept that rule but fixed the *real* defect underneath it — in a bare pack position the
trailing comma was **silently** absorbed into `[<function>, b]`; it is now a clear parse error (v1.13
A02-1). So the "no packing" verdict stood, but the silent-misparse it masked did not.
