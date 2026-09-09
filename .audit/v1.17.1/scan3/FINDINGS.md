# v1.17.1 — full-codebase deep audit, ROUND 3 (from scratch)

Fresh full sweep of the entire codebase, run from scratch per `.audit/deep_audit.md` and disregarding
all prior audit rounds. 8 parallel subsystem auditors (A1–A8) + orchestrator re-verification. Every
finding reproduced on a real binary and marked CONFIRMED; per-subsystem detail in `A1..A8_*.md`.

This round enforced the **user-code-provable hard rule**: a finding counts only if demonstrable with a
`.ki` program or the public C++ embedding API — the reproducer is the proof. Read-only "looks wrong"
observations are recorded as informational, never counted or fixed.

A deliberate expansion this round: the sanitizer presets (`asan`/`tsan`) are **TLS-OFF**, so the
OpenSSL-gated surface (14 `#ifdef KIRITO_ENABLE_TLS` blocks in `stdlib_crypto.hpp` + HTTPS in
`stdlib_net.hpp`) had never been exercised under Address/UBSan. Per maintainer instruction ("equal
scrutiny for the OpenSSL path"), an **asan+UBSan build WITH OpenSSL** (`build-bin/ki-asan-tls`) was
produced and A6/A7 re-probed the crypto + TLS handshake paths against a live localhost TLS server.

Interpreter `kVersion` stays **1.17.1** (audit/patch round).

## CONFIRMED findings — FIXED (4; zero HIGH)

- **A4 [MED] — `tensor.arange` fractional-step endpoint leak** (`stdlib_tensor.hpp` `arange`).
  `arange(0.0, 1.0, 0.1)` produced **11** elements (NumPy: 10), the 11th being `0.999… < stop`;
  `arange(1.0, 0.0, -0.1)` leaked a spurious `~1.4e-16` tail. The element count was pinned correctly by
  `ceil((stop-start)/step)`, but the fill loop used running accumulation `for(x=start; x<stop; x+=step)`,
  so float drift appended an element past the reserved count and the exclusive-stop contract.
  **Fix:** emit `start + i*step` over the fixed count (NumPy's algorithm). Regression: `spec_tensor_arange.ki`.

- **A7 [MED] — HTTPS read loop swallowed a recv timeout as a clean EOF** (silent-failure class)
  (`stdlib_net.hpp` `httpExchange`). The `SSL_read` loop exited on any `n <= 0` without consulting
  `SSL_get_error`, so a `SO_RCVTIMEO` timeout (stalled / black-hole TLS peer) returned a **status-0 empty
  Response with no throw** — while the plaintext path correctly threw `recv failed`. `raiseforstatus()`
  does not fire at status 0, so the idiomatic caller silently proceeded on an empty body. Root cause was
  an SSOT/slop divergence: the correct classification already existed in `tlsRecvAll`, but the HTTP path
  reimplemented the loop without it. **Fix:** classify `SSL_get_error` — throw `HTTPS recv timed out` on a
  timeout (via new `netcompat::lastErrorIsWouldBlock()`), while still treating `close_notify` / bare FIN /
  OpenSSL-3 "unexpected EOF" as end-of-body (HTTPS bodies here are framed by `Connection: close`, so a
  peer close is the normal terminator — throwing there would regress real servers). Regression: the
  reactivated `spec_net_tls.ki` `/hang` case (below).

- **[MED] — `spec_net_tls.ki` was silently skipping (dormant coverage)** — the root cause A7 escaped
  earlier rounds. The script looked for its server at `tools/tests/net_tls_server.py`, but it lives at
  `tests/net_tls_server.py`; `server == None` → `finish()` printed the golden `OK net_tls` **vacuously in
  every build**, TLS-on debug/release included. This is the exact bug class the previous round fixed in
  `probe_http_client_abuse.ki` — but the identical instance here was missed, so the real TLS handshake
  had never actually run. **Fix:** corrected the path (test now genuinely handshakes: verify-off
  round-trip, verify-on self-signed rejection, verify-on trusted acceptance, + the new timeout case).

- **A8 [LOW] — `textwrap.dedent` didn't normalize whitespace-only lines** (`stdlib_kimodules.hpp`),
  contradicting its own "matching CPython" comment. `dedent("    a\n  \n    b")` → `"a\n  \nb"` (stray
  spaces kept) vs CPython `"a\n\nb"`. **Fix:** normalize whitespace-only lines to empty first (CPython's
  `_whitespace_only_re.sub('', text)` step), including the no-common-prefix path. Regression: extended
  `verify_textwrap.ki`.

## OpenSSL / TLS path — now given equal asan/UBSan scrutiny (was previously unreachable)

- **A6 crypto (ki-asan-tls):** AES-GCM matches a NIST vector; tampered tag/AAD/truncated-tag all throw
  (no silent plaintext); IV mandatory; GCM-only (no ECB default). RSA-2048/OAEP-SHA256 and ECDSA
  sign-verify round-trip, tamper→False, key-confusion rejected. PBKDF2 matches RFC 6070/7914, iterations
  required (guarded `<1` and `≥2^32`). Fuzzed with **zero asan/UBSan hits and zero false-accepts**:
  aesdecrypt ×8k, ecverify ×4k, rsaverify/PEM ×3k, x509parse ×2k (+ 23k serde blobs).
- **A7 TLS (ki-asan-tls, live localhost self-signed):** verification ON by default — self-signed
  **rejected**; hostname/IP-SAN mismatch **rejected** (`127.0.0.1` vs `CN=localhost`); trusted GET+POST
  succeed (full handshake + record layer + teardown, asan/UBSan-clean across 60 back-to-back handshakes);
  CRLF injection still rejected over TLS. `net.Response`/`Session` Handle-holders confirmed GC-barrier-
  correct under `KIRITO_GC_THRESHOLD=1` — closing A3's one open item.

## Verified CORRECT (probed deep, honest nulls) — the pass is provably deep
- **A1 frontend:** 0 bugs. int64 literal wrap, subnormal/overflow float parse, f-strings (nested/specs/
  raw/escapes), deep-nesting stack-overflow gates (throw, no asan crash), closure/slot layout, analyzer
  warnings, multi-axis subscripts, switch constant-folding with cross-type key distinction.
- **A2 execution/VM:** 0 bugs. int64 two's-complement (no UBSan), floor-div/mod signs, div/mod/pow-by-
  zero catchable, int↔float exact compare + hash/equals near 2^53/2^63, Integer(String) overflow
  fail-fast, try/catch/finally across every exit, f-string caps, round half-away-from-zero. (The asan
  deep-recursion SIGSEGV is a sanitizer redzoned-frame artifact — release/debug catch it cleanly.)
- **A3 GC/memory:** 0 bugs. 22 churn probes under `KIRITO_GC_THRESHOLD=1` with fresh non-interned values
  + a tsan race probe; every user-reachable Handle-holder barrier-correct (containers, closures,
  BoundMethod, autograd parents, multi-axis tensor slices, regex MatchVal, lazy iterators, generators,
  deque, cycles, sort-key snapshot); the "young handles buffered in plain C++ storage" class is defended.
- **A4 numerics/tensor:** autograd vs finite-difference + analytic across the full op set; BigInt
  bit-identical to CPython at huge magnitude; tensor `%`//` floor signs; multi-axis slicing incl.
  INT64_MIN/MAX steps (no negation UB); empty-axis reductions; einsum; det/inverse — all asan-clean.
- **A5 text/collections:** 0 bugs. UTF-8 never splits multibyte; Bytes strict (no %256); regex
  linear-time on classic ReDoS patterns; Dict/Set semantics; iterators lazy with memory-safe snapshot.
- **A6 serde/sys:** hash/HMAC/CRC KATs byte-match; JSON edges; base64/hex/zlib round-trip + junk
  rejection; path/argv NUL-poison blocked; serialize's code-execution-on-load documented + gated.
- **A8 .ki stdlib:** statistics numerics hand-checked; deque/heapq complexity timed (O(1)/linear, not
  full-sort); deepcopy of cycles/shared-refs; 19 semver cases; csv/xml lenient-but-safe.

## Residual notes (informational — not counted, not fixed)
- A7 INFO-2: request `timeout` is per-operation (`SO_RCVTIMEO`), not a wall-clock deadline — matches
  `requests` semantics, by design.
- A8: `csv.parse("\"\"")` (whole input = one quoted-empty field) returns `[]` vs an arguable `[[""]]`;
  ambiguous, `readcsv` unaffected — SUSPECTED, not counted (left for a maintainer call).
- A5: mutation-during-iteration snapshots rather than raising (deterministic); `center()` right-pads vs
  CPython's left — both worth a doc note, neither a bug.
- JSON int > int64 silently widens to a lossy Float (documented maintainer decision).
- Optional test-infra: under `KIRITO_SANITIZER_BUILD`, `maxCallDepth_` (vm.hpp) could be lowered so the
  asan gate exercises recursion-limit paths without the redzoned-frame SIGSEGV.
