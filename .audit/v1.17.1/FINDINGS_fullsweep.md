# v1.17.1 — full-codebase deep audit (findings & fixes)

Fresh full sweep of the entire codebase (8 parallel subsystem auditors + orchestrator), per
`.audit/deep_audit.md`. Every finding reproduced on a real binary (`build-bin/ki-{release,asan,tsan}`),
marked CONFIRMED. Per-subsystem detail in `scan/A1..A8_*.md`. The codebase is heavily hardened (three
prior rounds); this sweep found **2 real bugs, 2 behavior/coverage fixes, and 1 perf fix** — all fixed
below with regression tests + docs. Interpreter `kVersion` stays 1.17.1 (audit/patch round).

## CONFIRMED findings — FIXED

- **F-A4 [HIGH] — tensor `%` truncated instead of floor-mod.** `stdlib_tensor.hpp` `ewFloat`/
  `ewFloatScalar` used bare `std::fmod`, so `Tensor([-7.0]) % 3.0` → `-1.0` (correct: `2.0`), diverging
  from scalar Float `%`, Integer/BigInt `%`, and NumPy, and breaking the documented divmod identity.
  **Fix:** a SSOT `floorModD(x,y)` helper (sign-corrects like the scalar path); used by tensor%tensor
  and tensor%scalar. Regression: `spec_tensor_floormod.ki`.

- **F-A2 [MED] — `Integer("<decimal in [2^63, 2^64)>")` silently wrapped to negative.**
  `runtime.hpp` parsed the magnitude with `stoull` (rejects only ≥2^64) then bit-cast, so the whole
  `[2^63,2^64)` window wrapped (`Integer("18446744073709551615")` → `-1`) — and the comment falsely
  claimed "FAILS FAST". **Fix:** range-check base-10 magnitudes (`≤INT64_MAX`, or `≤2^63` when
  negative) → throw; hex/oct/bin keep their intentional full-width bit-pattern wrap
  (`Integer("0xFFFFFFFFFFFFFFFF")==-1`, still tested). Comment corrected. Regression: `cov_builtins.ki`.

- **F-A7 [MED] — `net.get`/`post` had no default request timeout.** `stdlib_net.hpp` defaulted
  `timeout=0` (block forever); a stalled/black-hole host hung the caller indefinitely. **Fix (per
  maintainer decision):** default **10 s** (bounds connect + each send/recv; `timeout: 0` opts back
  out). Impact-checked: nothing in the suite relies on the old default (TLS/abuse tests hit fast
  localhost or pass explicit timeouts; kpm passes its own 30 s). Regression: a `/hang` endpoint +
  no-explicit-timeout case in the (now-reactivated) `probe_http_client_abuse.ki`.

- **F-TEST [MED] — the HTTP-client-abuse suite was silently skipping** (dormant coverage).
  `probe_http_client_abuse.ki` looked for the server at `tools/tests/net_abuse_server.py`, but it lives
  at `tests/net_abuse_server.py` (and CMake runs the test from the repo root expecting exactly that) —
  so every run printed "abuse server not found" and passed vacuously. **Fix:** corrected the path;
  the suite now runs (6 abuse cases + the new timeout case) and passes. Added to the ctest watchdog list.

- **F-A5 [LOW, perf] — regex `groupString` rebuilt `utf8Starts` on every call.** `stdlib_regex.hpp:79`
  recomputed an O(subject) code-point offset table per `.group()`/`.groups()`/`.groupdict()`.
  **Fix:** reuse the subject `StrVal`'s own cached `codePointStarts()` (the string is immutable) — SSOT,
  O(1) amortized. Correctness unchanged (existing regex tests cover it).

- **F-A1 [LOW, doc] — `09-types.md:54`** illustrated 64-bit wrap with `0x1_0000_0000_0000_0000`, but the
  lexer has no `_` digit separators (the same section says so). **Fix:** dropped the underscores.

## Verified CORRECT (probed deep, honest nulls) — the pass is provably deep
- **A3 GC/memory:** exhaustive soak under `KIRITO_GC_THRESHOLD=1` + asan/tsan with fresh non-interned
  values (lazy iterators, promoted containers, card-table spill, autograd graph, `parallel.spawn`) —
  zero sanitizer hits; write barriers verified in every mutator.
- **A2 execution:** int64 boundary arithmetic (two's-complement, no UBSan), div/mod-by-zero, exact
  int↔float compare, try/finally, recursion guards, f-string caps — all sound.
- **A1 frontend:** deep-nesting guards prevent stack overflow (parser gate tighter than all later
  passes); closure/slot layout, pure-only constant folding correct.
- **A5 text/collections:** regex linear-time (no ReDoS), Bytes strict (no `%256`), HashDoS-seeded
  buckets, reentrancy guard asan-clean, lazy iterators truly short-circuit.
- **A6 serde/sys/crypto:** MD5/SHA-1/2/HMAC/PBKDF2/AES-GCM/RSA/ECDSA byte-match published vectors; no
  weak KDF defaults; serde + compression asan-fuzzed (thousands of blobs, 0 crashes); path/argv NUL.
- **A7 net/parallel:** dispatcher zero-tsan; cross-VM copy-isolation confirmed; TLS verifies by default;
  CRLF injection rejected; timeout kills the whole process group.
- **A4 numerics/tensor:** BigInt/pow/modinv/gcd/isqrt at huge magnitudes, autograd vs analytic
  derivatives, einsum/matmul/reductions — all correct.
- **A8 `.ki` stdlib:** statistics/semver/deque (amortized O(1) timed)/csv/base64/heapq — correct.

## Residual notes (not fixed — informational)
- No compile-time guard on the `gcNeedsRootRescan` "stack-only" invariant (object.hpp) — a future
  footgun, not a current bug.
- JSON `int > int64` silently widens to a lossy Float (documented, asymmetric vs `stringify(BigInt)`).
- `serialize`/`dump` execute embedded class/function source on load (documented pickle-analogy warning).
