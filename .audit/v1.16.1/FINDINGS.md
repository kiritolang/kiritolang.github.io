# v1.16.1 — findings roster & status

Live status of the scan. Each agent writes its own `scan/A<NN>_*.md`; this table tracks them and
collects the CONFIRMED, actionable findings for the fix phase.

## Agent status
| Agent | Area | File | Status |
|-------|------|------|--------|
| A04 | GC: arena/pool/object/handle/rooting/write-barrier/card-marking | scan/A04_gc.md | launched |
| A05 | collections + lazy iterators + generators | scan/A05_collections_lazy.md | launched |
| A01 | lexer / parser / f-strings | scan/A01_lexer_parser.md | pending |
| A02 | resolver / analyzer / compiler / bytecode | scan/A02_compiler.md | pending |
| A03 | bytecode_vm / runtime / operators / exceptions / control | scan/A03_vm_runtime.md | pending |
| A06 | strings / bytes | scan/A06_strings_bytes.md | pending |
| A07 | numerics (Integer/Float/Complex/BigInt) + math | scan/A07_numerics.md | pending |
| A08 | classes / dunders / function / super / privacy / env / locals / module | scan/A08_classes.md | pending |
| A09 | builtins + value.hpp C++ API | scan/A09_builtins_api.md | pending |
| A10 | serde (json / serialize / dump) | scan/A10_serde.md | pending |
| A11 | io / path / sys / time / hash / crypto / zlib / gzip | scan/A11_sysmods.md | pending |
| A12 | net / parallel / dispatcher / compat | scan/A12_net_parallel.md | pending |
| A13 | regex / tensor / matrix / complex | scan/A13_engines.md | pending |
| A14 | ki-modules (stdlib_kimodules) | scan/A14_kimods.md | pending |
| A15 | perf variance sweep | scan/A15_perf.md | pending |
| A16 | coverage parity (C++ vs .ki gaps) | scan/A16_coverage.md | pending |

## Agent runtime note
The background subagents ARE productive but slow (~10 min, ~157k tokens each); their `.md` grows only
near the end (they analyze, then write), so a short progress poll looks like a stall. A04 + A05 both
delivered real findings. Given usage cost, remaining areas are being audited directly/selectively.

## Confirmed actionable findings

### FIXED this round
- **F05-1 [Med] — `x in range(...)` signed-overflow UB** (runtime.hpp RangeVal::contains). `(x - start_)
  % step_` overflowed for a range spanning >½ the int64 domain (reachable within the 32M cap via a large
  step, e.g. `0 in range(INT64_MIN, INT64_MAX, 6e12)`). **FIX:** unsigned offset math matching count()/
  at(). Regression in `spec_v1161_adversarial.ki` (runs under the asan/UBSan preset). Verified real +
  fix asan/UBSan-clean.
- **F04-1 [Med] — CardTable spill path (>8192 entries) untested** (object.hpp). Coverage gap in new code
  (arithmetic hand-verified correct, no functional bug). **FIX:** added a >9000-entry List/Dict/Set
  survival test under `setGcThreshold(1)` in test_gc_generational.cpp (exercises the spill vector).
  asan-clean.
- **F04-2 [Low] — List clear() didn't reset the CardTable** (Dict/Set did). Memory-safe but a perf/SSOT
  wart. **FIX:** single-sourced `ListVal::clearElems()` (resets elems + cards); both `list.clear()` and
  the value.hpp wrapper call it. Regression added (clear-then-refill an old >9000 list).

### FIXED — batch 2 (A03/A07/A08 findings)
- **F03-1 / F07-3 [Med] — float `%` wrong for large operands / NaN for inf divisor.** `x - floor(x/y)*y`
  collapsed the remainder (`1e17 % 3` → 0, want 1) and gave NaN for `5.0 % inf`. **FIX:** `std::fmod` +
  floor sign-correction (runtime.hpp numericBinary). Regression `spec_v1161_fixes.ki`.
- **F07-8 [High] — min/max/sorted couldn't order BigInt** (a NativeClass with kind()==Instance failed the
  `_lt_` dynamic_cast). **FIX:** kiLessThan dispatches to `binary(Lt)` for any Instance-kind (user `_lt_`
  OR a native ordered type); Complex/Socket still throw their own clear "unordered". Regression added.
- **F08-2 [High] — deserialized instance lost `_bool_` truthiness** (serde::rebuild set hash/eq flags but
  not bool → silent: a restored falsy instance became truthy; hit dump + serialize + parallel). **FIX:**
  single-sourced `InstanceValue::cacheDunderFlags()` (F08-4), called from callFull AND rebuild. Regression.
- **F07-1 [Med] — math.trunc returns Float** — RE-VERIFIED as **NOT A BUG / WONTFIX**. `trunc → Float`
  is a DELIBERATE, documented (`10-stdlib`), and explicitly-TESTED contract (7 golden scripts assert it
  "per doc", incl. `trunc(NaN)`/`trunc(large)` staying Float). Changing it to Integer broke all 7 +
  the doc — a contract, not a defect. The agent's flag was a Python-conformance note. Reverted; kept as
  a documented intentional divergence from Python. (Lesson: double-check "inconsistency" findings against
  the tested/documented contract before treating them as bugs.)

### FIXED — batch 2 (cont.)
- **F07-2 [Med] — BigInt == Float non-transitive + Set/Dict poison.** `BigInt(3)==3.0` was False though
  `3==3.0` and `3==BigInt(3)` are True, and a BigInt Dict key was unreachable by the equal Float. **FIX:**
  BigIntVal::equals gains a Float branch (exact within int64 range; symmetric via kiEquals fallback).
  Regression in spec_v1161_fixes.ki. asan-clean.

### OPEN — from batch 3 (agents DIED early on the session limit — scans PARTIAL, resume next session)
- **F02-1 [Med] CONFIRMED — analyzer false-positive** (analyzer.hpp): a local captured by an EARLIER-defined
  nested function (or mutual recursion — `isEven`/`isOdd`) is spuriously warned "assigned but never used".
  Program RUNS CORRECTLY (resolver resolves by membership); only the analyzer lacks a pre-declare pass.
  Fix: give analyzeBlock a collectBlockDecls-style pre-pass, or track used-before-declared per scope.
  A real fix worth doing next session; full repro in scan/A02_compiler.md.
- **A14 (ki-modules): 1 finding logged** in scan/A14_kimods.md (agent died after itertools, before tabular).
- **A01 (lexer/parser) + A12 (net/parallel): NOT audited** (agents died with ~0 output). Re-run next session.
- **F07-9 [Med] deferred** — sum() rejects BigInt/Complex. The naive fix (left-fold via applyBinaryOp from
  start=0) is WRONG: the reflected-operator rule makes `Integer(0) + BigInt` throw. A correct fix must seed
  the accumulator from the first non-scalar element (or require a matching start). Do carefully next session.

### FIXED — batch 3
- **F07-9 [Med] — sum() rejected BigInt/Complex.** Now a generic Handle accumulator (aux-rooted; native
  type on the LEFT per the reflected-operator rule) kicks in when a non-scalar element appears; scalar
  fast path unchanged. Regression in spec_v1161_fixes.ki; asan gc=1 clean (first attempt had a rooting
  bug — RootScope inside the streamIterate callback got popped by streamIterate's inner scope; fixed with
  a dedicated aux-root region).

### FIXED — batch 4 (user-directed)
- **F07-9 [Med] — sum() contract regression CLOSED (the correct, narrow fix).** The batch-3 generic-fold
  seeded on ANY non-scalar → it started concatenating Strings/Lists, breaking the deliberate "sum expects
  numbers"/"sum start must be a number"/"no list start concat" contract (r4_builtins, audit_builtins,
  labx_builtins, r8_docs). FIX: the generic accumulator now only engages for `ValueKind::Instance`
  (BigInt/Complex/user `_add_`); String/List/Bytes elements OR start re-throw the numeric-contract error.
  Regression: test_audit_v1161.cpp + spec_v1161_fixes.ki (rejects sum of Strings/Lists + a non-number start).
- **F01-1 [Med] — DECIDED (user): reject chained comparison at parse time.** `parseComparison` now consumes
  at most ONE comparison-family operator (==/!=/</<=/>/>=/in/not in); a second one throws "chained comparison
  is not allowed; connect the conditions with 'and'/'or'". Prevents `a<b<c` silently parsing as `(a<b)<c`.
  Tests: tests/errors/chained_comparison_{lt,eq,in} + test_audit_v1161.cpp. Migrated the old
  `X <cmp> Y == True/False` idiom in the golden scripts to the parenthesized form (`(X <cmp> Y) == True`);
  full 352-fixture golden suite green. Docs: 02-language-guide (new "Comparisons do not chain" note).
- **F08-1 [Med] — DECIDED (user): documented, not code-changed.** isinstance / typed `catch` / enforcing
  annotations match user classes by NAME not identity (needed for serde class-reconnection across VMs).
  Documented the limitation in docs/pages/08-builtins.md (isinstance) and 12-exceptions.md (typed catch).
- **F01-2 [Med]** `var a, = x` / `for x, in xs` drop the trailing comma (bind whole iterable, swallow count
  mismatch) while `a, = x` correctly 1-tuple-unpacks — silent path inconsistency. Real bug, nichey syntax.
- **F14-2 [Med]** Counter.mostcommon(negative n) returns an end-slice instead of `[]` (silent wrong vs CPython).
- **F14-4 [Med]** DataFrame([[...]]) from rows silently TRUNCATES a too-long later row (data loss); raw
  "index out of range" on a too-short one — inconsistent with readcsv (rejects/pads).
- **F14-1 [Med, pre-existing]** statistics.quantiles tail extrapolation. **F14-3/F14-5 [Low]** deque
  maxlen/rotate missing; bare Dict-error diagnostics in merge/agg.
- **F12-1 [Low]** HTTPS client accepts a TLS-truncated response silently (SSL_read<=0) unlike sibling
  paths that throw — TLS-truncation robustness. F12-2/F12-3 [Low] Barrier broken-state / Windows socketpair.
- **F09-1..7 [Low]** Integer("2^63") wraps (overflow boundary is 2^64), Float("1e400") throws, round()
  MSVC buf, value.hpp minor rooting/iterator/doc items. No High/Med in A09.
- **F13-1/F13-2 [Low]** regex negative count; tensor t[i] grad warnDetach. **F08-1 [Med]** isinstance by
  name not identity. **F08-3 [Low]** `!=` standalone `_ne_` symmetry. **F07-4/5/6 [Low]** BigInt<Float msg.

### OPEN — lower priority (candidates for later batches, all logged in scan/*.md)
- **F07-9 [Med]** sum()/min/max reject BigInt/Complex though `+`/operators work → fall back to
  applyBinaryOp(Add). **F07-2 [Med]** BigInt==Float non-transitive + Set/Dict bucket poison → BigIntVal::
  equals Float branch. (Both real, deferred to a numeric-boundary batch.)
- **F08-1 [Med]** isinstance/typed-catch/annotations match user classes by NAME not identity (two
  same-named classes conflate) — soundness hole, but name-matching is partly deliberate (serde
  reconnection); needs identity-first-with-name-fallback or a doc note.
- **F08-3 [Low]** `!=` not symmetric for a right-only standalone `_ne_`. **F13-1 [Low]** regex.sub/split
  negative count treated as unlimited (Python-divergence). **F13-2 [Low]** tensor `t[i]` grad path lacks
  warnDetach. **F07-4/5/6 [Low]** BigInt<Float message / Float("1e400")→throw / modpow negative modulus.
  **F07-7** NaN Set/Dict key — DESIGN (exact ==), document only. CardTable::any() dead code.
### PERF (A15 — proposals, need release-build measurement before applying)
- P15-1 harness should report median+p95 (not mean+stddev over a bimodal dist) — low-risk/high-reward.
- P15-4 skip the aux-root rescan via a live-IterCursor counter (low-risk; my v1.16.0 code).
- P15-2/P15-3 decouple major threshold from nursery / raise the major floor — Med-risk, measure first.
- P15-5/P15-6 warm-up reps / operand-stack reserve. (Variance root: rare large GC pauses on random reps.)
- **F05-2 [Low]** Dict/Set entry positions are `int32_t` — silent probe corruption past 2^31 entries
  (~64 GB, impractical; no clean-error guard like the range/repeat caps). Consider a guarded throw.
- **F05-3 [Low]** map/filter/zip/enumerate are RE-iterable (fresh cursor per pass) vs Python's one-shot.
  Deliberate design choice (friendlier); documented divergence, not a bug. No action unless we want strict
  Python parity.
- **F05-4 [Low]** `filter(None, xs)` throws "not callable" instead of Python's truthiness filter. Feature
  gap; low value.
- **A04 coverage notes**: CardTable::any() has no C++ caller (dead code — remove or use); over-alignment
  pool path + slot-generation-wraparound are defensive/unreachable (0% coverage, acceptable).

### Areas audited, judged SOUND (no fix)
- A05 lazy/collections/generators broad adversarial+pathological+boundary pass — clean (see
  scan/A05_collections_lazy.md + `spec_v1161_adversarial.ki`).
- A04 generational invariant, barrier/remembered lockstep, ProbeScope reentrancy, pool size-class,
  gcNeedsRootRescan promotion — sound (see scan/A04_gc.md).
