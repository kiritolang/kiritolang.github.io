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

## DEFERRED — needs a maintainer decision (NOT auto-fixed)

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

## STILL OPEN (triaged, not yet fixed) — candidates for the next batch

Higher-value remaining, roughly by severity:
- A02-3 (HIGH): duplicate parameter name desyncs resolver env layout vs runtime frame layout
  (debug: assertion abort; release: silent wrong binding). NOTE: check whether the analyzer's
  "duplicate parameter names" warning already rejects — if so this may be narrower than stated.
- A10-2 (HIGH, build): `kirito.hpp` doesn't compile under clang++ default flags → documented
  libFuzzer build dead. Build/portability, not a runtime bug.
- A14-1 (MED): `Tensor.take()` with no args reads past the argument span (OOB read).
- A07-1-adjacent / A19.1-1 (MED): `_hash_`/`_eq_`/`_bool_` dispatch via `activeVM()` — misroutes with
  2+ VMs on a thread. C++ probe written by A19.1 agent, could not compile.
- A04-2 (MED): a bound method backed by a NATIVE function silently discards keyword arguments.
- A09-1 / A09-2 (MED): nested function inside a method loses class ownership (can't touch
  `self._private`, `self._super_()` unavailable).
- A02-1 (MED): compiler hidden locals (`$with0`/`$exc0`) leak into a module's public exports.
- A13-1/A10-4, A13-2 (MED): eager class-var initializer cross-class / captured-instance load in a
  fresh VM.
- A14-3 (MED): tensor `all`/`any` disagree with themselves over a zero-length axis.
- A11-1 / A11-2 (MED, perf): xml entity decode and base64.encode are O(n²).
- A15-1 / A15-2 (MED): bad cwd misreported; embedded NUL truncates argv/shell/cwd.
- A01-1 / A01-4 (MED): inline Function line-continuation unserializable; inline body rejects
  `discard` the analyzer recommends.
- A04-1 (MED): traceback frame line disagrees with `error:` line on `finally`/`with` reraise.
- Plus the LOW batch (A01-2/3, A05-1..4, A08-1/2/3, A09-3/4, A10-3/4, A13-3, A14-2/4, A18-1, A06-2/3).

## Meta findings (not code bugs)
- A20-0: previous inventory was 22% of the real surface (see `scan/A20_surface_*.txt` for the full map).
- A08-3: `.audit/README.md` false-positives table misdescribes the v1.12 A11-1 `math.trunc` verdict.
