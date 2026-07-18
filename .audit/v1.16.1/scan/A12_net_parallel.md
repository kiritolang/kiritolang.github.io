# A12 — net + parallel/dispatcher + compat (v1.16.1 re-run)

Scope: dispatcher.hpp, stdlib_parallel.hpp, stdlib_net.hpp, net_compat.hpp, proc_compat.hpp, rand_compat.hpp.
Verdict up front: this is careful, well-hardened concurrency code. The deadlock/abort design, lock
ordering, Task reap coordination, RAII on sockets/pipes/SSL, and the no-weak-entropy contract all hold
up under scrutiny. Findings below are genuine but small; no High.

---

### F12-1 [Low] HTTPS response loop silently accepts a truncated stream (TLS truncation)
- src/kirito/stdlib_net.hpp:711-717 (net::httpExchange, KIRITO_ENABLE_TLS path) — the read loop is
  `while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0) { raw.append(...); }`. Any `n <= 0` exits the loop
  and the function returns the partial `raw` with NO error, regardless of whether it was a clean
  `SSL_ERROR_ZERO_RETURN` (close_notify) or a hard error / unexpected TCP FIN mid-record.
- Why: this is inconsistent with BOTH sibling paths, which DO distinguish: plain `net::recvAll`
  (line 286) throws on `n < 0`, and the socket-level `SocketVal::tlsRecvAll` (line 136-146) checks
  `SSL_get_error`==ZERO_RETURN and `throw`s on `got < 0`. Only this HTTP-client inline loop swallows the
  error. A network attacker able to inject a FIN can truncate an HTTPS body and the client accepts it
  silently (no Content-Length check either). Impact is bounded because it is a deliberately-simple
  `Connection: close` client, but it is a real integrity gap and an inconsistency.
- Trigger: hard to reproduce without a live TLS server that drops mid-stream; the code path is plain
  by inspection.
- Fix: mirror tlsRecvAll — after the loop, inspect `SSL_get_error(ssl, n)`; treat `SSL_ERROR_ZERO_RETURN`
  as clean EOF and throw on anything else (or reuse tlsRecv/tlsRecvAll so the two can't drift again).
- Verified-real: YES (by code inspection; contrast with lines 286 and 136-146 in the same file).

### F12-2 [Low] Barrier "broken" state is not sticky across generations (differs from Python)
- src/kirito/dispatcher.hpp:280-302 — when a timeout breaks generation `g` it sets `brokenGen_ = g` and
  advances `generation_`/`count_ = 0`. A subsequently-arriving party enters the NEW generation, which is
  not marked broken, so it `cv_.wait`s normally instead of immediately getting `Broken`. Python's
  threading.Barrier stays broken for all subsequent `wait()`s until `reset()`.
- Why / impact: not a deadlock or safety bug — a late arrival that also carries a timeout will itself
  time out and re-break. But a late arrival with NO timeout (Barrier.wait() with timeout=None) after a
  partial break will block until enough fresh parties re-accumulate, rather than surfacing the broken
  barrier. This is a semantic surprise, not a hang for the timeout-driven usage.
- Trigger: N-party barrier, some parties time out and break it, a remaining party then calls
  `wait()` (no timeout).
- Fix (if desired): keep a persistent `broken_` flag set by timeout/abort and cleared only by
  `resetBarrier()`, and have `wait()` return `Broken` immediately while it is set. Otherwise document
  the auto-recycling semantics.
- Verified-real: YES (logic inspection). Deadlock unit suite exercises Barrier x3 but this nuance is
  behavioral, not liveness.

### F12-3 [Low] Windows socketpair emulation does not authenticate the accepted peer
- src/kirito/net_compat.hpp:199-222 (`socketPair`, _WIN32) — emulates a pair via a 127.0.0.1 ephemeral
  listener + connect + accept. It does not set SO_REUSEADDR and, more importantly, never verifies that
  the `accept`ed `server` fd is actually the peer of `client` (e.g. via matching getsockname/getpeername).
  Another local process racing a `connect()` to the ephemeral port between `listen` and `accept` could be
  the one that gets accepted.
- Why: Python's own Windows socketpair emulation guards against exactly this (it checks the connected
  peer address). Local-only, best-effort, Windows-only; still a confused-deputy risk for a "share-nothing
  pair" primitive that `parallel`/servers may lean on.
- Trigger: a hostile/buggy local process connecting to the transient loopback port during the emulation.
- Fix: after accept, compare `getpeername(server)` to `getsockname(client)` (and vice-versa); reject a
  mismatch. Optionally bind the listener and connect from a known-good source.
- Verified-real: YES (code inspection). POSIX path uses native `socketpair(AF_UNIX)` and is fine.

### F12-4 [Info] Bare-blocking-syscall abort gap is the ONLY hole — confirmed
- A worker blocked in a primitive (Queue/Lock/Event/Semaphore/Barrier) or in `Task.join` is always
  reachable by teardown: `shutdown()` (dispatcher.hpp:627-641) sets `shuttingDown_`, `abort()`s every
  live Waitable, THEN reaps tasks; `Task.join`'s `thread.join()` is not a Waitable but shutdown reaps
  every Task directly, and the `joining`/`done`-spin coordination in `reap()` (654-660) makes even a
  transitive parent→child join chain resolve once the leaf unwinds. The genuinely-unabortable cases are
  exactly the documented ones: a bare `socket.recv/accept`, `io.input`, `sys.shell` with no timeout —
  none of these route through a Waitable, so a worker parked in one hangs `shutdown()`'s join. Confirmed
  no additional gap; matches CLAUDE.md's documented boundary. Advice to give such calls a timeout is
  correct (settimeout enforces `>= 0`, stdlib_net.hpp:1216-1230; negative rejected).
- Verified-real: YES (traced all five primitives + Task lifecycle + reap coordination).

### F12-5 [Info] Lock ordering, entropy, and socket RAII verified clean
- Lock order is single-direction registryMutex_ → primitive m_ only via `makeWaitable`'s pre-abort
  (dispatcher.hpp:665-679); no primitive method ever takes registryMutex_, so no inversion (TSan-gated).
  `shutdown()` collects live waitables under the lock then aborts them WITHOUT it (628-634), also clean.
- `rand_compat.hpp` fillRandom returns false only when EVERY source failed and never emits weak/zero
  bytes; partial getrandom correctly continues into /dev/urandom from the filled offset (46-80). No
  silent-fallback path. Caller-must-throw contract is honored by design.
- SocketVal RAII: `closeFd()` tears down SSL then fd; `detach()` refuses a TLS socket and clears fd so a
  second detach/fileno can't leak a stale handle; `fdOrThrow` gives a clean "socket is closed" instead of
  a raw errno; starttls refuses non-stream/closed/already-TLS and throws (never silent plaintext) on a
  no-TLS build. proc_compat drains stdout/stderr on separate threads (no pipe deadlock), kills the whole
  process group/Job on timeout, and never lets a drain-thread bad_alloc escape (SIGABRT-safe).
- Verified-real: YES.
