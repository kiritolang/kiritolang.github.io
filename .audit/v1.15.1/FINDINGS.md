# Audit v1.15.1 — triage roll-up + fix log

Scan phase: 20 agents, 44 findings (see `scan/AXX_*.md`). This file tracks the fix phase.

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
