# Kirito Deep Audit — Orchestrator Brief

## Mission
Audit the entire Kirito codebase (C++ header-only core AND the Kirito-implemented `.ki`
standard library) as if it were CRITICAL INFRASTRUCTURE: one wrong result, one UB, one
memory/race bug, one silent data-corruption path, or one insane default could kill people.
Depth over breadth-of-claims: every finding must be REPRODUCED against a built binary and
marked CONFIRMED vs SUSPECTED. "I read it and it looks fine" is not coverage — probe it.

Scope = all changes since the last audited release (diff the range) PLUS a fresh full sweep
(regressions hide in untouched code that a change now exercises differently).

## What to hunt (every dimension — do NOT stop at the C++ core)

1. CORRECTNESS / LOGIC BUGS
   - Wrong results on edge/empty/single/huge/negative/duplicate/invalid/NaN/Inf/-0 inputs.
   - Off-by-one, boundary, overflow (int64 wrap, 2^63 cast), precision loss, sign conventions.
   - Documentation-vs-reality divergence (run every doc example; compare behavior to docs/).

2. MEMORY / UB / CONCURRENCY (C++ core)
   - UAF, OOB, use-after-realloc (references into vector/map held across an allocation), signed
     overflow, narrowing casts, aliasing, data races in the parallel dispatcher. Validate with
     asan AND tsan actually running the adversarial tests — not by reading.
   - GC invariants: every native object holding Handles must root/barrier its young children;
     verify each holder. Interned small ints mask bugs — probe with fresh non-interned values.

3. BAD ARCHITECTURE / WRONG DATA STRUCTURE
   - The "Complex-Matrix-as-List-of-Lists instead of native tensor" class of disaster: a type
     whose backing structure violates its contract's complexity (e.g. a `deque` backed by a
     List so both-end ops are O(n) and FIFO use is O(n^2)). Verify each container/algorithm
     actually meets its advertised big-O — measure it (2x input => ~2x, not ~4x).

4. BAD / INSANE DEFAULTS, SIDE EFFECTS, SURPRISES
   - Unsafe default args (a KDF with tiny iterations, a network call with NO timeout, a lossy
     default formatter used on data). Hidden mutation, non-determinism, order-dependence.

5. SILENT FAILURE / SILENT DEGRADATION (a top-priority class)
   - Any path that swallows an error and returns a plausible-but-wrong value: broad `catch(...)`
     that returns the input, an aggregation that silently drops non-numeric/dirty data, a codec
     that coerces out-of-range input (byte %256), a parser that accepts junk glued to a token,
     duplicate keys/columns that silently merge and fabricate rows. Fail fast and loud instead.

6. COUPLING / DECOUPLING (C++ and .ki API)
   - Should-be-coupled-but-isn't: duplicated logic/constants/messages that can drift (SSOT
     violations); the same concept defined twice in C++ and .ki that can silently disagree; two
     code paths computing "the same" value differently.
   - Should-be-independent-but-isn't: hidden global mutable state, business logic tangled with
     I/O, a module reaching into another's internals.

7. AI-INDUCED "SLOP" / DIVERSIONS (hunt explicitly)
   - A feature Y that SHOULD reuse a proven feature X but instead reimplements it slightly
     differently, adds a workaround/fallback "just in case", or special-cases what should be
     generalized. Two functions doing one conceptual thing divergently (e.g. two hex encoders,
     two parsers, nsmallest-via-heap vs nlargest-via-sort, a heapsort mislabeled a k-way merge).
   - Logic in the WRONG file/module. A comment that lies about what the code does. Helper function
     living in source that should be generalized utility function.

8. THE .ki STANDARD LIBRARY / KIMODULES — DEEP, not shallow
   - Read the FULL Kirito source of every module (itertools, functools, collections/deque/
     Counter/defaultdict, heapq, bisect, statistics, string, textwrap, csv, tabular
     (Series/DataFrame), xml, enum, arg, semver, copy, base64, tee, …). For EACH: are the
     algorithms correct AND idiomatic AND right-complexity? Probe adversarially. Verify numeric
     results against an INDEPENDENT hand computation. Check laziness/eagerness, tie-breaking,
     alignment, deep-copy of cycles/shared refs, parser leniency-vs-corruption, escape
     correctness. This layer is memory-safe but full of logic/complexity/idiom bugs.

## Check against the project's own code rules (CLAUDE.md "General Programming Rules")
SSOT; no silent failures/fallbacks; KISS; no UB; no global mutable state; structured/diagnostic
errors (what/where/context, not bare strings); separate concerns; prefer pure functions; minimize
coupling/maximize cohesion; validate inputs at boundaries; preserve invariants; no implicit/unsafe
conversions; document non-trivial complexity. Flag every violation. Also: "never assume a request
(or the existing code) is correct" — flag a better alternative rather than silently complying.

## Coverage requirement (this is half the job)
Find MISSING TESTS — and value MISSING KIRITO (.ki) TEST SCRIPTS at least as much as missing C++
tests. Every module/feature needs adversarial/edge/empty/invalid/boundary coverage:
error-path tests (.experr), golden tests (.expected), property/fuzz where applicable, deterministic
seeds for RNG, independent verification for numeric. Every confirmed bug gets a regression test
NAMED FOR THE SYMPTOM, in whichever surface(s) exercise it (C++ unit AND/OR .ki).

## Docs
Keep docs in lockstep with any change: the Kirito language/stdlib reference, the C++ embedding-API
reference, the course pages, AND the EXCEPTION LIST — every error message added or changed must be
in the exceptions reference. Regenerate the built doc bundle. Run every doc code-fence as a test.
Compare docs against implementation (whether all described in docs is true to code) and implementation
against docs (whether everything is documented).

## Language-design gaps
Note missing capabilities that force awkward workarounds (e.g. no multi-axis slicing `t[:, 2:4]`,
no instance slice protocol). Propose a clean design; flag it as its own scoped feature round.

## Only user-code-provable bugs count (hard rule)
A finding is in scope — to be counted, reported as a bug, and fixed — ONLY if it can be provably
demonstrated with **user code**: either a Kirito (`.ki`) program, or C++ that uses the public embedding
API (the Value API and the documented `KiritoVM`/`Object` surface — not private internals). The
reproducer IS the proof: the exact `.ki` or embedding-API snippet plus its actual-vs-expected output
(or the asan/UBSan/tsan trace it triggers). If a defect exists in the source text but CANNOT be reached
from any valid user program or supported embedding call (UB in a branch no user input can reach, an
internal helper with no caller path from user code, a wrong result behind an unreachable guard), it is
OUT OF SCOPE: do not fix it and do not count it as a bug — at most note it once as an informational
"unreachable/internal" observation. No theoretical, read-only, or "looks wrong but I can't trigger it"
findings. The test is simple: *show the user code that breaks.* If you can't write it, it isn't a bug.

## Method & discipline
- Reproduce everything on a real binary. For iteration use the ASAN build; run the FULL 4-variant
  gate (debug/release/asan/tsan) only at the very end. Reuse already-compiled binaries to run new
  .ki tests when source is unchanged. NEVER run two builds at once (RAM). Cap sanitizer jobs.
- Fan out parallel auditors by subsystem; each OWNS its findings and reports concretely.
- Report format PER FINDING: file:line, severity (HIGH/MED/LOW), a concrete reproducer
  (input + actual-vs-expected), root cause, CONFIRMED vs SUSPECTED. Also list what was verified
  CORRECT (so the pass is provably deep, not shallow). The orchestrator RE-VERIFIES every
  high-severity finding personally before acting. Fixes: minimal, SSOT, with a regression test and
  docs, each behavior-preserving where it must be. Get a human decision on judgment calls
  (behavior changes, rename-vs-throw, substantial rewrites) before implementing them.
- Commit to `claude-branch` in logical commits; tests + docs land WITH each fix; version bump is a
  PATCH for an audit/hardening round; no release/tag/PR unless explicitly asked.
- Don't needlessly recompile: if changes are only to .ki test files and no source files,
  reuse previously compiled binaries. Prefer testing with ASAN/TSAN.
