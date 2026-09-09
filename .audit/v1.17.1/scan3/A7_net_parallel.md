# A7 — NET / PARALLEL — deep audit (scan3, v1.17.1)

Scope: `net` HTTP client + socket layer (`stdlib_net.hpp`, `net_compat.hpp`) and the `parallel`
dispatcher (`stdlib_parallel.hpp`, `dispatcher.hpp`). Binaries reused: `build-bin/ki-asan`,
`ki-release`, `ki-tsan` (NOT rebuilt). Network kept to localhost only (`tests/net_abuse_server.py`).

## Headline

Pass 1 (no-TLS binaries): **zero CONFIRMED bugs** in the reproducible surface. Pass 2 (ki-asan-tls, TLS
built in): TLS certificate verification, hostname checking, and the trusted handshake/teardown are all
**correct** (verify on by default, self-signed and hostname-mismatch both rejected, no way found to make
`net.get` accept a bad cert), but I found **1 CONFIRMED MED**: the HTTPS `SSL_read` loop in
`httpExchange` silently swallows a mid-stream read error/timeout (returns a status-0 empty Response)
where the plain-TCP path throws — a silent-truncation / parity gap (**TLS-1** below).

Every other user-code-reproducible behavior in scope is correct: header/CRLF injection rejected (over
plaintext AND TLS), URL parsing rejects malformed input, the default 10 s request timeout bounds a
black-hole host, the redirect loop is capped, net.Response/Session are barrier-correct under GC stress,
and the parallel dispatcher is **TSAN-clean** under every adversarial concurrent program, with cross-VM
copy-isolation holding.

---

## Findings

### INFO-1 — HTTPS/TLS path is not reproducible in the pass-1 binaries (SUPERSEDED by pass 2)
`stdlib_net.hpp:648-745` (TLS code behind `#ifdef KIRITO_ENABLE_TLS`)

> **UPDATE:** closed by the `ki-asan-tls` pass — see "## TLS-enabled path (ki-asan-tls)" below. The
> static reading was borne out (verify defaults true, hostname check real), and one behavioural bug
> surfaced only by execution (TLS-1). The pass-1 note is retained for provenance.


The audit binaries are built **without** `KIRITO_ENABLE_TLS`: `net.tlsenabled` returns `False`, and any
`https://` call throws `"https requires building with KIRITO_ENABLE_TLS"`. Reproducer:

```
var net = import("net"); io.print(net.tlsenabled)   # -> False
```

Therefore the brief's TLS items ("verification on by default", "verify=False bypass", cert-hostname
checking) **cannot be proven or disproven with user code on these binaries** — per the hard rule they are
out of scope for a CONFIRMED/SUSPECTED verdict here. Static reading of `tlsClientHandshake`
(`stdlib_net.hpp:672-709`) shows verify defaults to `true`, sets `SSL_VERIFY_PEER` +
`SSL_set1_host` + `X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS`, and re-checks `SSL_get_verify_result` after
the handshake — i.e. it looks correct — but I did not execute it. Recommend a TLS-enabled binary in a
future scan to close this coverage gap.

### INFO-2 — Request timeout is per-operation, not a total deadline (by design, NOT a bug)
`stdlib_net.hpp:295-308` (`setTimeout` → `SO_RCVTIMEO`/`SO_SNDTIMEO`) + `net_compat.hpp:133-185`
(`connectWithTimeout`)

The 10 s default bounds the connect and **each** individual send/recv. A peer that dribbles bytes
slower than the connection would idle but faster than the timeout window keeps the transfer alive past
the nominal timeout. Measured against `/slow-body` (20 bytes, 80 ms apart) with `timeout: 1`:

```
slow-body timeout=1: handled=True elapsed=1.60s   # streamed the whole 1.6s body; 1s per-op never tripped
```

This matches Python `requests` semantics (its `timeout` is also per-read, not a wall-clock deadline) and
the in-code comment ("a slow-but-progressing transfer is unaffected"). It is a deliberate design choice,
not a defect; noting it because a true wall-clock cap would be needed to fully defeat a slowloris drip.
No total-deadline API exists, which is a reasonable YAGNI call. Not counted.

---

## Verified CORRECT (reproduced)

### net — header/CRLF/request-smuggling defenses (ki-asan)
All reject **before** any socket I/O:
- Header value CRLF → `header contains a control character (CR/LF): 'X-Evil'`
- Header key CRLF → thrown
- URL with raw CR/LF → `URL contains a control character` (also protects server-controlled redirect
  `Location`, re-parsed each hop)
- Cookie name/value CRLF → `cookie contains a control character (CR/LF): 'sid'`
- Query `params` containing CR/LF are **percent-encoded** (correct — not header fields), so the request
  is built and only fails at connect.

### net — URL parsing (ki-asan)
- `http://h:99999/` → `port out of range` ✓
- `http://h:abc/` → `invalid port` ✓
- `ftp://h/` → `URL must start with http:// or https://` ✓
- `urlsplit("http://user:pass@[::1]:8080/p?q=1#f")` → host `[::1]`, port `8080`, userinfo stripped,
  query/fragment split correctly ✓
- `urlsplit("https://ex.com/a/b?x=y")` → clean split ✓

### net — timeout bounds a hanging host (ki-release, localhost `/hang`)
```
hang default-timeout: threw=True  elapsed=10.23s   # 10s default applied, did NOT hang forever
hang timeout=2:       threw=True  elapsed=2.02s    # explicit timeout honored
```
The `/hang` endpoint accepts then never replies; connect succeeds and the bounded recv fires. Confirmed
the default is finite and connect+recv are both bounded.

### net — redirect loop is capped (ki-release, localhost `/loop` → 302 self-redirect)
`net.get(".../loop")` (default `maxredirects: 10`) returns `status=302` after the cap in ~3 ms — no
infinite recursion, no stack blowup. ✓

### parallel — dispatcher is TSAN-clean under adversarial concurrency (ki-tsan)
All programs run under `TSAN_OPTIONS=halt_on_error=0`; **zero** ThreadSanitizer warnings, correct
results. Stress-repeated (5×) with no flake.

| Program | What it stresses | Result |
|---|---|---|
| `par_queue` | 8 producers + 1 consumer on a bounded (maxsize 64) MPMC Queue, 4000 items | total=998000 == expected, TSAN clean |
| `par_lock` | 6 workers, non-reentrant Lock guarding a read-modify-write of a shared counter (via Queue), 1800 incs | counter=1800 exact (no lost updates), TSAN clean |
| `par_barrier` | 8 parties, two-phase rendezvous | all 8 released each phase, TSAN clean |
| `par_sem` | Semaphore(3) bounding 20 workers + Lock + Queue peak-tracking | TSAN clean |
| `par_event` | 10 workers blocked on Event.wait, one set() releases all | all 10 released, TSAN clean |
| `par_nested` | 12 workers each nested-spawn a leaf task and join it | sum=144 exact, nested spawn/join clean, TSAN clean |
| `par_isolate` | worker mutates a List + Dict passed via spawn | worker saw len=4; **main's `[1,2,3]` and `{a:1}` unchanged** — copy-isolation holds |
| `par_shutdown` | 6 workers block on an empty Queue.get; main exits without feeding | dispatcher shutdown aborts all blocked workers cleanly, TSAN clean, exit 0 |

Cross-VM isolation (`par_isolate`) confirms values cross by serialize/deserialize
(`dumpfmt::write`/`read`), so a worker cannot alias the parent's mutable objects — no shared-arena
aliasing. Coordination primitives (Queue/Lock/Event/Semaphore/Barrier) cross by identity via
`_getstate_`/`_setstate_` dispatcher ids, exercised throughout with no race.

Shutdown-with-blocked-workers (`par_shutdown`) confirms the designed-in deadlock safety: `shutdown()`
aborts every `Waitable` before joining threads, so workers blocked on `Queue.get` unwind and the process
terminates cleanly instead of hanging.

---

## Not reproduced / out of scope
- `net.get`/`net.post` TLS verification, hostname checks, handshake/teardown — now COVERED in pass 2
  (ki-asan-tls); see below.
- `starttls`/`cipher`/`is_tls` **socket-level** TLS (as opposed to the HTTP client) — not exercised; the
  shared `tlsClientHandshake` is covered via the HTTP path, but the `SocketVal::starttls` entry point
  and `tlsRecvAll`/`tlsSendAll` byte I/O were not directly driven. Minor remaining gap.
- Windows-specific paths (`addWindowsRootCerts`, winsock `socketPair` emulation) — Linux host.

## Bottom line
Pass 1: no confirmed bugs across the plaintext net client, URL/injection hardening, timeouts, redirects,
and the parallel dispatcher (TSAN-clean under queue/lock/event/semaphore/barrier/nested-spawn/shutdown
stress, correct cross-VM isolation). Pass 2 (ki-asan-tls): TLS verification/hostname/handshake all
correct and unbypassable in my testing, Response/Session GC-barrier-correct, no asan leak across 60
handshakes — **one CONFIRMED MED (TLS-1)**: the HTTPS read loop swallows a read timeout/truncation that
the plaintext path throws on. Recommend the orchestrator re-verify TLS-1 and fix by reusing the hardened
`tlsRecvAll` error classification in `httpExchange`.

---

## TLS-enabled path (ki-asan-tls)

Second pass on `build-bin/ki-asan-tls` (`net.tlsenabled == True`, OpenSSL 3.0.13, asan+UBSan). Localhost
TLS server: self-signed RSA-2048 cert, `CN=localhost`, `subjectAltName=DNS:localhost`, served via a
Python `ssl`-wrapped `http.server`. INFO-1 from pass 1 is now closed for these items.

### TLS-1 — HTTPS read loop silently swallows a mid-stream read error / timeout (MED, CONFIRMED)
`stdlib_net.hpp:733-738` (the `SSL_read` loop in `httpExchange`)

```cpp
char buf[4096]; int n;
while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0) {
    raw.append(buf, ...);
    if (raw.size() > net::kMaxRecvAll) throw ...;
}
// <-- no SSL_get_error() check here: any n <= 0 (timeout, RST, truncation) is treated as clean EOF
```

The loop exits on any `n <= 0` and never inspects `SSL_get_error`, so a **recv timeout** or a
**mid-stream truncation** is silently accepted as a complete response. The plain-TCP path
(`net::recvAll`, `stdlib_net.hpp:281-292`) instead throws `"recv failed"` on `n < 0`. The *correct*
TLS discipline already exists elsewhere in the same file — `SocketVal::tlsRecvAll`
(`stdlib_net.hpp:136-146`) returns 0 only on `SSL_ERROR_ZERO_RETURN` and throws `"SSL_read failed"` on a
negative — but `httpExchange` reimplements the read loop *without* that check (an SSOT / AI-slop
divergence: two TLS read loops, one hardened, one not).

Reproducer (localhost TLS server whose `/hang` accepts + handshakes but never replies; `verify=False`):

```
# plain HTTP black-hole, timeout=2:
PLAIN: threw (recv failed: Resource temporarily unavailable) elapsed=2.02s
# HTTPS hang, timeout=3:
returned status=0 bodylen=0  elapsed=3.07s      # <-- NO throw, silent status-0 Response
```

Actual: HTTPS timeout/truncation → a status-0, empty-body `Response`, no exception. `r["ok"]` is `False`
(0 ∉ [100,400)), but `raiseforstatus()` does **not** fire (it only raises `>= 400`), so a caller using
the idiomatic `net.get(...).raiseforstatus()` silently proceeds with an empty body. Expected: parity with
the plaintext path — a thrown, catchable timeout/read error (or at minimum a distinct exception on a
truncated TLS stream). Root cause: missing `SSL_get_error` classification after the read loop.

Impact is bounded (the response is clearly status-0/empty, not a plausible-but-wrong 200), so MED, not
HIGH. It is a genuine silent-degradation + plain/TLS inconsistency and a truncation-attack surface (an
active network attacker who RSTs mid-body gets the partial body accepted as complete). Fix: after the
loop, `SSL_get_error(ssl, n)` and throw on `SSL_ERROR_SYSCALL`/`SSL_ERROR_SSL` (and, to fully match
`tlsRecvAll`, require `SSL_ERROR_ZERO_RETURN` for a clean EOF) — reuse the one hardened loop rather than
duplicating it. Suggested regression test name: `https_read_timeout_throws_like_plaintext`.

### Verified CORRECT — TLS (ki-asan-tls, localhost server)

- **Verify ON by default — self-signed REJECTED.** `net.get("https://localhost:8443/x")` →
  `TLS handshake with localhost failed: self-signed certificate (... pass verify=False)`. No silent
  accept. ✓
- **verify=False opt-in works.** `net.get(..., {"verify": False})` → `status=200 body=hello-tls-body`.
  Full handshake + record layer + teardown exercised under asan/UBSan, no trap. ✓
- **Trusting the cert via `SSL_CERT_FILE=/tmp/c.pem` + SAN match succeeds.** `https://localhost:8443/...`
  → `status=200`, correct body, `X-Echo-Path` header echoed. Confirms `SSL_set1_host` accepts a matching
  SAN and `SSL_CTX_set_default_verify_paths` honours `SSL_CERT_FILE`. ✓
- **Hostname/IP mismatch REJECTED even when the cert is trusted.** Cert trusted, connect to
  `https://127.0.0.1:8443/` (127.0.0.1 not in `SAN=DNS:localhost`) →
  `TLS handshake with 127.0.0.1 failed: IP address mismatch`. Proves hostname verification is real, not
  just chain-of-trust. ✓
- **POST over trusted TLS succeeds** (`net.post(..., {"data": {...}})` → 200), exercising SSL_write +
  request framing. ✓
- **CRLF/header injection still rejected over TLS**, before any I/O:
  `net.get("https://...", {"verify": False, "headers": {"X-E": "a\r\nInjected: 1"}})` →
  `header contains a control character (CR/LF): 'X-E'`. ✓
- **Timeout bounds a hanging TLS peer** — 3 s timeout unblocked a 20 s-sleeping TLS `/hang` in 3.07 s
  (does NOT hang to the server's 20 s). The *bound* works; only the *reporting* is wrong (TLS-1). ✓
- **No asan/UBSan leak/UAF across many handshakes.** 60 back-to-back
  `net.get("https://localhost:8443/hN", {"verify": False})` all returned 200, clean exit, zero asan
  output (each Socket owns + frees its `SSL`/`SSL_CTX` and closes the fd in `closeFd`). ✓

### Verified CORRECT — net.Response / net.Session under GC stress (ki-asan-tls, `KIRITO_GC_THRESHOLD=1`)

Closes the A3 GC gap for the Handle-holders at `stdlib_net.hpp:776` (`ResponseVal::children` →
headers/cookies Dicts) and `:813` (`SessionVal::children`).

- **net.Response barrier-correct under GC.** Loop of 40 `net.get` calls, holding every 4th `Response` in
  a list while churning 50 fresh List allocations per iteration (GC threshold 1 → collect on nearly
  every alloc); then read all 10 held Responses' `.statuscode` / `.text` / `.header("x-path")` back:
  `kept=10 mismatches=0`, no asan trap. The headers Dict child is rooted through `children()` and
  survives collection — no swept-child wrong value. ✓
- **net.Session barrier-correct under GC.** 30 `Session.get` calls with per-iteration Dict churn under
  threshold 1; session `headers["X-App"]` and cookie jar read back intact afterward, and a final request
  returns 200 with the correct body. ✓
