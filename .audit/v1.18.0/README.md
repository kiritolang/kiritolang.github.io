# Audit round v1.18.0

A full-codebase deep-audit round on the (unreleased) `1.18.0` base, run after clearing ten deferred
debt items from prior rounds. Scope = the 1.17.1→1.18.0 diff (20 commits — heavy on VM/tensor/regex
perf fast paths + the isprime redesign) PLUS a fresh full sweep of the whole C++ core and the
Kirito-implemented stdlib. Version stays **1.18.0** (an audit/hardening loop, no feature/breaking work);
no release/tag.

## Method

Six adversarial auditors fanned out by subsystem, each reproducing every finding on a **pre-built**
binary (`build-debug/ki` for logic, `build-asan/ki` for memory/UB, `KIRITO_GC_THRESHOLD=1` for GC
soak) — no auditor was allowed to build (concurrent builds OOM the box). Only **user-code-provable**
findings count (a `.ki` program or the public C++ embedding API showing actual-vs-expected). The
orchestrator re-verified every finding on a real binary before acting, got a maintainer decision on
each behavior change, then fixed with regression tests (C++ **and** `.ki`) + docs in lockstep.

The numeric/tensor/VM territory — under-audited because an earlier sweep rate-limited out — got a
dedicated from-scratch correctness hunt (see `scan/A1_numeric_tensor_vm.md`).

## Outcome

11 findings triaged (0 HIGH, 5 MED, 4 LOW, 2 INFO). No memory/UB/GC/race bug was found — the C++
core's safety surface re-verified clean under ASan+UBSan with forced GC. Every confirmed defect was a
**silent-failure / SSOT / docs-vs-reality** issue in the higher layers. Maintainer decisions: fix all
four silent-failure paths loudly (#1/#2/#3/#6); reject NaN Dict/Set keys (#7); unify `Tensor.round()`
to the scalar half-away-from-zero convention (#9). See `FINDINGS.md` for the triaged roll-up and
`scan/` for the per-subsystem raw reports (findings + what was verified CORRECT).

The four rc=0 "false-positive" categories the orchestrator's first suite run flagged were a harness
artifact (merged stderr), not defects. The real fallout of #7 (NaN-key rejection) reached internal
stdlib code — `tabular.Series.unique`/`isin` keyed column values into a Dict/Set — and was fixed to be
NaN-safe (collapse NaN to one distinct value / skip it in the query set), not just papered over in tests.
