# Audit round v1.16.1

Deep, comprehensive hardening audit of the entire Kirito codebase, following the `v1.12`–`v1.15.1`
rounds. Kicked off after v1.16.0 (lazy generators + ordered collections + GC card marking shipped in
PR #31, on `claude-branch`). This round is a **PATCH/hardening** round — `kVersion` bumps to `1.16.1`
only if/when fixes ship and the user asks.

## Goals (from the maintainer)
1. **Find bugs** — semantic/logical correctness, weak spots — via deep static analysis + reading.
2. **Test everything from every angle** — not one method/attribute/class/argument/feature untested.
   Adversarial, fuzz, edge, typical, random, malformed inputs. C++ tests first (check coverage, extend),
   then `.ki` tests.
3. **SSOT** — every reused behavior has one authoritative implementation; flag duplication.
4. **Perf variance** — Kirito's perf is OK but has high stddev; find LOW-RISK / HIGH-REWARD fixes only.
5. Every fix: double-check it was truly a bug; stay idiomatic; break no contract; ship
   edge/fuzz/adversarial tests; update docs.

## How agents record work (CRITICAL — usage may run out mid-flight)
Every scan agent OWNS one file `scan/A<NN>_<area>.md` and **writes findings there incrementally as it
goes** (checkpoint after each finding), so a force-stopped worker loses nothing. Finding format:

```
### F<NN>-<n>  [SEVERITY High|Med|Low]  <one-line title>
- file:line — <what & why it's a bug / weak spot / gap>
- Repro / trigger: <concrete input or scenario>
- Fix idea: <minimal, idiomatic remediation>
- Test to add: <adversarial/edge/fuzz case>
- Verified-real: <yes/no — did I confirm it's actually a bug, not intended?>
```
Also keep a running `## Coverage notes` section: which methods/attributes/args of your area are
UNtested (C++ and .ki), so the test-extension phase has a checklist.

## Phase plan
- **Phase 1 — SCAN** (this batch): agents read + statically analyze their area, log findings + coverage
  gaps to their `.md`. NO edits, NO builds (read-only; a build may be running).
- **Phase 2 — TEST EXTENSION**: fill the coverage gaps (C++ then .ki), adversarial/fuzz/edge.
- **Phase 3 — FIXES**: apply the verified findings, each with a regression test; re-verify each was real.
- **Phase 4 — PERF**: low-risk variance reductions.
- Gate (`tools/scripts/post_work_check.sh`) between phases where build-affecting.

## Agent roster → its file
See `FINDINGS.md` for the live status table. Areas: A01 lexer/parser/f-string, A02 resolver/analyzer/
compiler/bytecode, A03 vm/runtime/operators/exceptions/control, A04 GC/arena/pool/object/handle/rooting/
barrier/cards, A05 collections/lazy-iterators/generators, A06 strings/bytes, A07 numerics(Integer/Float/
Complex/BigInt)/math, A08 classes/dunders/function/super/privacy/env/locals/module, A09 builtins/value.hpp
C++ API, A10 serde(json/serialize/dump), A11 io/path/sys/time/hash/crypto/zlib/gzip, A12 net/parallel/
dispatcher/compat, A13 regex/tensor/matrix, A14 ki-modules, A15 perf-variance, A16 coverage-parity.
