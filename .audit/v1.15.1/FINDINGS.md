# Audit v1.15.1 — triage roll-up + fix log

Scan phase: 20 agents (3 — A14/A17/A18 — were cut off by a session wipe and resumed to completion),
44+ findings (see `scan/AXX_*.md`). This file tracks the fix phase.

**Validation gate at HEAD 98cd047 (both batches): debug 820/820, release 820/820, asan 820/820,
tsan 820/820 — all green.** (The manual asan/tsan run first needed the sanctioned environment —
`ulimit -s 262144` for asan's deep-recursion tests, `setarch -R` for tsan's ASLR — without which
every tsan test FATALs on "unexpected memory mapping"; that was an invocation artifact, not a code
defect.)

## FIXED (this session) — batch 1: HIGH + memory/security correctness

All verified live on `build-debug/ki` (rebuilt), each with a regression that FAILS on the pre-fix
build. Regression tests: `tools/tests/unit/test_audit_v1151.cpp`.

| ID    | Sev  | Symptom | Fix | File(s) |
|-------|------|---------|-----|---------|
| A06-1 | HIGH | `Dict.popitem()` segfaults / can't drain a Dict with a NaN key | `DictVal::popArbitrary()` takes the pair straight from a bucket (a NaN key is unfindable by equality), mirroring `SetVal::popArbitrary` | collections.hpp, runtime.hpp |
| A03-1 / A10-1 | HIGH | `Dict.update(<user _iter_ returning a fresh container>)` hands out a dangling handle (fires at DEFAULT cadence, ~1/12500 calls) | root every level via the single-sourced `rootedIterate` under a `RootScope`, exactly like the set-algebra family | runtime.hpp |
| A08-4 | HIGH | BigInt/Integer Dict+Set key equality asymmetric → two `==`-equal keys in one Dict, order-dependent | `probeBucket` gains the symmetric-equality retry `kiEquals` already uses; gated on kind mismatch so write-only NaN keys are untouched | collections.hpp |
| A16-1 | HIGH | `hash.pbkdf2` silently truncates `iterations` to 32 bits (2^32+1 → single HMAC) | range-check against the `uint32_t` actually used before narrowing | stdlib_hash.hpp |
| A07-1 | MED  | `Bytes._setstate_` / `BigInt._setstate_` re-home an immutable hashable value in place → corrupt Dict/Set keyed on it | one-shot `initialized_` guard mirroring DateTime; the no-arg value ctors construct *initialized* empties, leaving the default ctor to the deserializer | bytes.hpp, stdlib_int.hpp, runtime.hpp |

Note A08-4: the fix makes the code match CLAUDE.md's existing claim ("BigInt hashes equal to an equal
native Integer (shared Dict/Set bucket)") — no doc change; the docs were already correct, the code lagged.

## FIXED (this session) — batch 2: MED correctness + usability, low-risk

All verified live; regression cases added to `tools/tests/unit/test_audit_v1151.cpp`.

| ID    | Sev  | Symptom | Fix | File(s) |
|-------|------|---------|-----|---------|
| A14-1 | MED  | `Tensor.take()` with no arg reads past the empty arg span (OOB) | `Args(vm,a,"take").require(1)` before touching `a[0]`, matching every sibling method | stdlib_tensor.hpp |
| A14-5 | MED  | in-place `t[i,j]=v` on a grad-tracking tensor silently desyncs autograd → gradients vs stale values | `setItem` refuses when `requiresGrad || node` (PyTorch parity; matches the documented functional-update style) | stdlib_tensor.hpp |
| A02-1 | MED  | compiler hidden temporaries `$with0`/`$exc0` leak into module exports (inspect noise + a top-level `with` retains its ctx mgr forever) | both export filters drop `$`-prefixed names via a single-sourced `moduleExportBase` helper | runtime.hpp |
| A18-5 | MED  | `tabular` masking `df[df["col"] > v]` throws on any blank cell (which readcsv makes from empty fields) — the headline idiom | `Series._binop` propagates missing (None/NaN) to a falsy mask entry instead of calling op on it | stdlib_kimodules.hpp |
| A04-2 | MED  | a bound method backed by a NATIVE fn silently drops every keyword arg (and skips arity/default checks) | route `makeBoundMethod` through the single `applyCall` dispatch (kwargs/bindArgs/arity) | runtime.hpp |
| A14-6 | LOW  | `softplus` overflows to inf for x>~709 (naive `log1p(exp(x))`) | stable `max(x,0)+log1p(exp(-|x|))` (NumPy/PyTorch form) | stdlib_tensor.hpp |
| A08-2 | LOW  | `math.prod` throws "result too large" for a product that is exactly 0 (sticky overflow flag) | a zero factor resets the running state (0 × anything = 0) | stdlib_math.hpp |
| A16-2 | LOW  | `File.writelines(<write-only stream>)` leaks a raw `bad optional access` | check the `iterate` optional; throw a clear "argument must be iterable" | stdlib_io.hpp |

## FIXED (this session) — batch 3: process-exec safety + tensor error/alloc + doc accuracy

Regression cases in `test_audit_v1151.cpp`; each verified live.

| ID    | Sev  | Symptom | Fix | File(s) |
|-------|------|---------|-----|---------|
| A15-2 | MED  | an embedded NUL silently truncates an argv element / shell command / cwd (poison-NUL bypass: validation & execution disagree) | reject a NUL in argv/command/cwd in the single `runExternalProcess` funnel (input stays byte-faithful), as CPython does | stdlib_sys.hpp |
| A15-1 | MED  | a bad `cwd` was misreported as "failed to start '<program>'", blaming a program that exists | POSIX child sends a stage tag (chdir vs exec) + errno; parent + the Windows branch report a directory error distinctly | proc_compat.hpp |
| A14-2 | LOW  | `Matrix`/`ComplexMatrix` `determinant()` leaked a raw `tensor::TensorError` (no line:col), unlike `inverse()` | wrap the engine call, matching `inverse` | stdlib_matrix.hpp, stdlib_complex.hpp |
| A14-4 | LOW  | `tensor.arange` allocated ~1 GB before rejecting an oversized range | bound the element count up front (`ceil((stop-start)/step)`), reject non-finite; keep the in-loop guard as a backstop | stdlib_tensor.hpp |
| A05-3 | LOW  | `Bytes()` accepts any iterable of Integers but its message + docs said "a List of Integers" | reword the message + docs to "an iterable of Integers (0..255)" (code was correct leniency) | bytes.hpp, docs/pages/09,12 |

## FIXED (this session) — batch 4: GC timing-stability (A12, low-risk, zero semantic change)

The user's perf ask was variance, not throughput. A12 measured minor GC at ~53% of a sum_loop run
and isolated three leads. The two zero-semantic-change ones are fixed here; the third (the big one)
is deferred as it needs a redesign, which the user's "low-risk only" rules out.

| ID  | Symptom | Fix | File(s) |
|-----|---------|-----|---------|
| A12-L1 | every collection malloc/frees (and page-faults) its ~256 KB mark-stack from scratch — a per-minor jitter source | hoist `work`/`childbuf` to reused VM members `gcWork_`/`gcChildbuf_` (`clear()` keeps capacity); safe to share minor/major (single-threaded, collections never nest) | vm.hpp |
| A12-L2 | every major makes a second full O(capacity) pass (`liveCount()`) right after `sweep()` just to retarget | `sweep()` returns the survivor count it already walks past; the retarget uses it | vm.hpp, arena.hpp |

Correctness: pure optimizations, so there is no behavioural regression to assert — semantic
equivalence is proven by the whole suite + the `--gc-threshold 1` soak staying green under **all four
variants (debug/release/asan/tsan 820/820)**. A flaky timing test would be the wrong thing to add.

**DEFERRED — A12-L3 (the big one, needs a redesign, NOT low-risk):** every collection rescans
`tempRoots_` in full, so a native that accumulates N objects under one `RootScope` (range, sorted,
map, zip, split, join, …) is O(N²/nursery) in root visits — `range(3M)` inside a timed loop is the
worst case. The cure A12 proposes is an adaptive nursery (its P1) and/or rooting the container once
instead of each element; both are cadence/hot-path changes that exceed the user's "low-risk only"
bound. Flagged with full measurements in `scan/A12_perf_variance.md`.

## FIXED (this session) — batch 5: a self-contradiction + two O(n²) hot paths

Regression cases (correctness — a timing test would be flaky) in `test_audit_v1151.cpp`.

| ID    | Sev | Symptom | Fix | File(s) |
|-------|-----|---------|-----|---------|
| A14-3 | MED | tensor `all`/`any` return the identity whole-tensor but THROW per-axis over a zero-length axis — the same function disagreeing with itself (and with NumPy) | pass the identity (all→True, any→False) to `reduceAxis`, exactly as `sum`/`prod` do | stdlib_tensor.hpp |
| A11-2 | MED | `base64.encode` was O(n²) (`out = out + ch` per char) — 32 KB took ~5.7 s, ~1 MB ~an hour | build a List, `"".join` once (the module's own `decode`/`csv` idiom). Measured: 32 KB 5.7 s → 0.27 s, now linear | stdlib_kimodules.hpp |
| A11-1 | MED | `xml` text `_decode` same O(n²) — one `&` in a big text node made decoding ~500× slower | same List+join rewrite | stdlib_kimodules.hpp |

## FIXED (this session) — batch 6: LOW correctness + DRY + doc accuracy

| ID  | Symptom | Fix | File(s) |
|-----|---------|-----|---------|
| A08-1 | `Integer.compare(other, 0.0, 0.0)` returned True for two int64 differing by 1 above 2^53 (lossy double round-trip) | exact `__int128` diff when both operands are Integer | runtime.hpp |
| A09-4 | a class with `_setstate_` but no `_getstate_` silently deserialized half-initialized (`_setstate_` never ran) | reject at flatten time, mirroring the `_getstate_`-without-`_setstate_` hard error | stdlib_serde.hpp |
| A05-1 | `range`'s guard borrowed the byte-sized `kMaxRepeat`, admitting a ~21 GB list (per-element ~80 B) | new `kMaxRangeCount` (~32M elems ≈ 2.5 GB), sized for range's real cost | common.hpp, runtime.hpp |
| A10-3 (DRY) | the "row index out of range" text existed 4× with two spellings — `matrix`/`complex` `row()` dropped the type prefix `getItem` uses | align both to "Matrix/ComplexMatrix row index out of range" | stdlib_matrix.hpp, stdlib_complex.hpp |
| A10-4 / A05-4 (DRY) | `kMaxRepeat` re-typed as the literal `256*1024*1024` in the random/bytes result-length guards, one with a comment falsely claiming the constant is "not visible here" | use `kMaxRepeat`; fix the stale comment | stdlib_random.hpp, bytes.hpp |
| A01-3 (doc) | CLAUDE.md called positional-after-keyword a compile-time diagnostic; it is a deferred, catchable runtime throw | corrected | CLAUDE.md |
| A08-3 (doc) | `.audit/README.md`'s FP table said `math.trunc` returns Integer; it returns **Float** (the row inverted the v1.12 verdict) | corrected + warned against "restoring" an Integer return | .audit/README.md |
| A05-2 (doc) | `isinstance(v, type(v))` is False for a class value (type() shares a name with instances) — undocumented | noted under `type`/`isinstance` | docs/pages/08-builtins.md |

## FIXED (this session) — batch 7: the 4 conformance changes (maintainer-APPROVED after the pinned-test gate)

The user (maintainer) reviewed the four findings that each overturned a deliberately-pinned test and
approved applying ALL of them, updating the pinned tests to the conformant behaviour. Verified live +
targeted tests (the relevant golden scripts, all 43 regex scripts, and the tabular/regex/json/serde/
collections C++ unit tests) all green.

| ID    | Change | Pinned tests updated |
|-------|--------|----------------------|
| A18-5 | tabular `Series._binop` propagates missing (None/NaN)→None instead of throwing (pandas parity; `df[df["col"]>v]` survives a blank cell) | deep_tabular.ki, r4_kimods_b.ki |
| A17-3 | regex `must_advance`: findall/finditer/sub/split no longer drop a non-empty match masked by a higher-priority zero-width one. Added a defaulted `mustAdvance` to `reng::run` (rejects a zero-width match at exactly startPos); stays linear-time | r8_net_regex.ki |
| A18-1 | `deque.pop()`/`popleft()` on empty say "pop from an empty deque", not the internal "List" | verify_collections.ki |
| A13-3 | `json.loads` U+FFFD-substitutes a high surrogate + non-low `\u` (rewind `pos_-=6`), consistent with a truly-lone surrogate | r6_json, audit_json(+.expected), labx_serde, r4_serialization, deep_serialization, r8_serde_data |

Docs updated: json exceptions table (drop the now-unreachable "invalid low surrogate"), tabular Series
missing-propagation note.

## FIXED (this session) — batch 8: A02-3 (HIGH) + A02-2, maintainer-approved

| ID    | Change | Tests |
|-------|--------|-------|
| A02-3 (HIGH) | a **duplicate parameter name** is now a hard PARSE error (parser.hpp), not a warn-and-run — killing the resolver-slot-layout desync (assertion abort / silent wrong binding) at the source. Analyzer's dead dup-param warning removed. | new `errors/duplicate_param.ki`; updated `test_warnings.cpp`, `r7_language.ki`; audit test |
| A02-2 | the two module-export filters are UNIFIED into one `moduleExportBase`: a `.ki`-file module now HIDES its `_private` top-level names, exactly like a frozen module already did (dunders `_x_` and ordinary names still export). No fallbacks, per the maintainer. | audit test (frozen path) |

## FIXED (this session) — batch 9: remaining involved MED

| ID    | Sev  | Symptom | Fix | File(s) |
|-------|------|---------|-----|---------|
| A19.1-1 | MED | `_hash_`/`_eq_`/`_bool_` on a user instance dispatched via `activeVM()` (the most-recently-constructed VM), misrouting when 2+ VMs coexist on a thread → stale-generation throw or silent type confusion (breaks the multi-VM isolation contract) | `InstanceValue` carries its **owning VM** (`ownerVM_`, set at instantiation + serde rebuild); the three slots dispatch through it, `activeVM()` only a null fallback | class_value.hpp, runtime.hpp, stdlib_serde.hpp |
| A10-2 | HIGH(build) | `kirito.hpp` didn't compile under clang++ default flags (the sized aligned `operator delete` needs `-fsized-deallocation`) → the documented libFuzzer build was dead | guard the sized overload behind `#if defined(__cpp_sized_deallocation)` (the unsized aligned delete already covers over-aligned); add `-fsized-deallocation` for Clang in CMake | object.hpp, CMakeLists.txt |
| A04-1 | MED | the innermost traceback frame line disagreed with the `error:` line whenever an exception crossed a `finally`/`with` (the frame showed the `try:`/`with` line, the reraised instruction, not the throw) | `appendFrame` prefers the exception's own origin span for the innermost frame (KiritoThrow arm only); outer frames keep the call site | bytecode_vm.hpp; new `errors/traceback_line_through_finally.ki` |
| A06-2 | LOW | `ValueKind::Array` is a reserved kind with no producer (no `ArrayVal`), only defensive `|| Array` checks — reader confusion | clarifying comment at the enum (kept as a reserved placeholder; removing it + its 9 `\|\| Array` sites is churn with no behavioural change) | object.hpp |
| A09-1 / A09-2 | MED | a function literal created INSIDE a method had no class ownership, so a nested closure/callback was DENIED `self._private` and `self._super_()` (contract: code inside a method is within that method) | a `MakeFunction` executed in a method frame inherits its owner class (fresh KiFunction per execution, so the "never stamp a shared fn" rule holds); a fn defined outside a method still can't reach a private | bytecode_vm.hpp |

## DEFERRED — needs a maintainer decision (NOT auto-fixed)

_(none remaining — all four conformance items were approved and applied in batch 7.)_

_Historical (now resolved): A13-3 was initially reverted at the golden-test gate because six json/serde
tests pinned the throw; the maintainer then approved the conformant behaviour, so it and the tests
were updated together._

- **(was) A13-3 (LOW): `json.loads` throws on a high surrogate followed by a valid-but-non-low `\u` escape**
  (`\uD800A`), while a truly lone high surrogate (not followed by `\u`) is U+FFFD-substituted.
  The scan called this an inconsistency with the code's own unpaired-surrogate rule — but **six tests**
  (`r6_json`, `audit_json`, `labx_serde`, `r4_serialization`, `deep_serialization`, `r8_serde_data`)
  deliberately pin the throw ("high surrogate + non-low/BMP `\u` throws"). Treating a *malformed pair*
  attempt more strictly than a lone surrogate is a defensible, tested choice; overturning it is a
  maintainer call. Reverted (the third scan finding this round to claim "inconsistent/untested" while
  a test pinned the behaviour — same lesson as A18-5/A18-1).

**Lesson repeated:** two scan findings (A18-5, A18-1) claimed the behaviour was "untested". It was NOT
— each contradicts an existing passing test, one with an explanatory comment. Reverted rather than
overturn a pinned decision. This is the round's own "double-check the bug was a bug" rule biting the
scanners: a "no test pins this" claim must be verified with `grep`, not asserted.

- **A18-5 (MED): `tabular` Series/DataFrame element-wise arithmetic & comparison THROW on a None/NaN
  operand**, so the headline masking idiom `df[df["col"] > v]` crashes whenever a column has a blank
  cell (readcsv makes None from an empty field). pandas propagates (None→NaN; NaN comparison→False→row
  drop). **But** `deep_tabular.ki:47-48` and `r4_kimods_b.ki:499` pin the throw *with a comment* ("a
  None can't take a binary operator"), and `r8_tabular.ki`/others rely on it — a deliberate design.
  Recommend: adopt pandas-parity missing-propagation in `Series._binop` + update those tests. Reverted.
- **A18-1 (LOW): `deque.pop()`/`popleft()` on empty reports "pop from empty List"**, leaking the
  internal List type. Three tests pin the `"empty List"` substring (`verify_collections.ki:60`,
  `labx_containers.ki`, `r4_collections.ki`) — incidental, but changing the message breaks all three.
  Recommend: "pop from an empty deque" + update the three assertions. Reverted (low value vs. churn).

- **A17-3 (MED): regex empty-match handling drops real matches (missing Python "must_advance").**
  `findall`/`finditer`/`sub`/`split` lose non-empty matches when a higher-priority zero-width match
  exists at a position (e.g. `regex.findall("|\\w","ab")` → `['','','']` vs Python `['','a','','b','']`).
  Well-evidenced (21k-case differential fuzz vs Python, 114 divergences all this shape) and the fix
  preserves linear time (RE2 implements must_advance). **But** it changes observable behaviour that an
  existing test explicitly pins — `tools/tests/scripts/r8_net_regex.ki:207` asserts the current divergent
  output with a justifying comment. Overturning a deliberately-pinned test is a maintainer call, so this
  is flagged, not silently fixed. Recommend: adopt Python-conformant must_advance + update that test.

## STILL OPEN (triaged, not yet fixed) — for a future session

Fixed across batches 1–5: A06-1, A03-1, A08-4, A16-1, A07-1, A14-1, A14-5, A02-1, A04-2, A14-6,
A08-2, A16-2, A15-1, A15-2, A14-2, A14-4, A05-3, A12-L1, A12-L2, A14-3, A11-1, A11-2 (22 fixes).
The remaining backlog, roughly by value — each deliberately left because it is either genuinely
involved (a core resolver/compiler change deserving a fresh, careful session) or a low-value tail:

**Involved — merit a dedicated session, not a tail-of-a-long-run rush:**
- A10-2 (HIGH, build): `kirito.hpp` doesn't compile under clang++ default flags → the documented
  libFuzzer build is dead. Build/portability, not a runtime bug.
- A09-1 / A09-2 (MED): a nested function inside a method loses class ownership (can't touch
  `self._private`; `self._super_()` unavailable) — closure/ownership plumbing.
- A13-1 / A13-2 (MED): an eager class-var initializer that calls across classes / reads a captured
  instance fails to load in a fresh VM — serde rebuild ordering.
- A04-1 (MED): traceback frame line disagrees with the `error:` line on a `finally`/`with` reraise.
- A19.1-1 (MED): `_hash_`/`_eq_`/`_bool_` dispatch via `activeVM()` misroutes with 2+ VMs on one
  thread (the A05-1 landmine). Needs a C++-level repro the scan agent couldn't compile.
- A01-1 / A01-4 (MED): an inline `Function` relying on a bracket line-continuation is unserializable;
  an inline body rejects the `discard` the analyzer recommends.

**LOW tail** (cosmetic / DRY / doc): A01-2 (f-string error col), A01-3 (doc), A05-1/2/4, A08-1
(`.compare` exact >2^53), A08-3 (FP-table doc), A09-3 (arity counts self), A09-4 (`_setstate_` w/o
`_getstate_`), A10-3/A10-4 (DRY: dedup the "row index out of range" text + `kMaxRepeat`), A13-3
(json surrogate), A06-2/3 (vestigial `ValueKind::Array`), A02-2 (export `_private` divergence).

## Meta findings (not code bugs)
- A20-0: previous inventory was 22% of the real surface (see `scan/A20_surface_*.txt` for the full map).
- A08-3: `.audit/README.md` false-positives table misdescribes the v1.12 A11-1 `math.trunc` verdict.
