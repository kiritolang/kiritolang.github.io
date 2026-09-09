# A7 — net / parallel / proc concurrency + security audit (v1.17.1)

Scope: `src/kirito/stdlib_net.hpp`, `net_compat.hpp`, `stdlib_parallel.hpp`, `dispatcher.hpp`,
`proc_compat.hpp` (+ the `sys` proc glue in `stdlib_sys.hpp`).
Method: source read + reproduction on the pre-built sanitizer/release binaries in `build-bin/`
(`ki-tsan`, `ki-asan`, `ki-release`). `ulimit -s 262144`. No source or tests were modified.
A TSan report would be a CONFIRMED race; none were produced by any probe below.

Binary under test: `ki (Kirito) 1.17.1`, TLS build ON (`net.tlsenabled == True`).

---

## FINDINGS

### F1 — HTTP client has NO default timeout; a silent/black-hole peer hangs the interpreter forever
- **Severity:** MEDIUM (availability / DoS-by-hang). CONFIRMED.
- **Where:** `stdlib_net.hpp:974` (`double timeout = 0.0;` in `netRequest`), applied via
  `httpExchange`→`dialTcp(u, 0.0)` (blocking connect, `net_compat.hpp:135`) and
  `setTimeout(fd, 0)` which is a no-op for `seconds <= 0` (`stdlib_net.hpp:296`), so `recvAll`
  (`stdlib_net.hpp:281`) blocks on `recv` with no `SO_RCVTIMEO`.
- **Reproducer:** loopback server that `accept()`s but never replies; client `net.get(url)` with no
  `timeout` option.
  - `/tmp/srv_silent.ki` (binds 127.0.0.1:18131, accepts, never sends).
  - `/tmp/cli_noto.ki`: `net.get("http://127.0.0.1:18131/")` → `timeout 6 ki-release` → **EXIT 124
    (killed, hung)**.
  - `/tmp/cli_to.ki`: same URL with `{"timeout": 1}` → clean catchable error in ~1s
    (`recv failed: Resource temporarily unavailable`).
- **Root cause:** the high-level `request`/`get`/`post`/… verbs default `timeout` to 0 ("no timeout"),
  which disables both the bounded connect and the send/recv `SO_*TIMEO`. A hostile or merely stuck
  server (accept-and-stall, or one that ignores `Connection: close` and holds the socket open past
  its Content-Length) blocks the calling VM thread indefinitely; there is no watchdog.
- **Note / mitigation:** this mirrors Python `requests` (also no default timeout — a well-known
  footgun) and is fully avoidable by passing `{"timeout": N}`. But for "critical infrastructure" a
  spawn-per-request server that omits the option is one bad upstream away from thread exhaustion.
  Recommend a sane default (e.g. 30 s) or at least documenting the requirement prominently.
  The socket-level `SocketVal.timeout = 0.0` default (`stdlib_net.hpp:78`) is standard blocking-socket
  semantics (Python parity) and is NOT flagged.

No other findings. Everything else probed came back correct — see below.

---

## Verified CORRECT (races I tried to induce and could not; guarantees confirmed on a real binary)

### Parallel dispatcher (the data-race surface) — all clean under TSan
- **Shared-primitive contention, no race.** 6 workers hammering one shared `Queue` + `Lock` +
  `Semaphore(3)` + `Barrier(6)`, 200 iters each (`/tmp/tsan_contend.ki`): `ki-tsan` **clean**, result
  deterministic (119400 = 6×Σ0..199). Same probe under `ki-asan` + `KIRITO_GC_THRESHOLD=1` **clean**.
- **Cross-VM value copy-isolation CONFIRMED.** A List passed to a worker via `spawn` and mutated there
  does not affect the parent (`/tmp/iso.ki`: worker len 4, parent stays `[1,2,3]`). Same through a
  `Queue` (`/tmp/qiso.ki`, also clean under `ki-tsan`+GC1): values cross by `dumpfmt` serialize/deserialize
  into each VM's own arena — no shared arena. Primitives (Queue/Lock/…) correctly cross by *identity*
  (dispatcher id via `_getstate_`/`_setstate_`) to the same mutex-guarded C++ object.
- **No deadlock on teardown of blocked workers.** 4 workers blocked forever in `Queue.get(block,None)`
  + 4 in `Barrier.wait(None)` (parties never met), main returns without joining (`/tmp/abort.ki`):
  process exits cleanly (`shutdown()` aborts every `Waitable` before joining). Clean under `ki-tsan`.
- **spawn/join churn, no reap/erase race.** 40 rounds × 8 spawn+join (`/tmp/churn.ki`): `ki-tsan`
  clean, deterministic (1440). `reap()`'s `joining`/`done` atomics correctly serialize join()-vs-shutdown().
- **done()-polling concurrent with a running worker** (`/tmp/done.ki`): `ki-tsan` clean.
- **Fork-bomb guard works.** A top-level `spawn` NOT behind `if argmain:` makes every worker fail with
  the documented diagnostic ("cannot spawn from a module's top level …") rather than fork-bombing
  (`/tmp/forkguard.ki`) — the `bootstrapping()` flag is per-worker-VM. Legitimate **nested** spawns
  (behind `argmain`, from inside a spawned fn) still work (`/tmp/nested.ki` → 21).
  Caveat (by design, not a bug): there is no guard against an *intentional* recursive `spawn(self)`;
  it is bounded only by OS thread limits, at which point `pthread_create` EAGAIN is caught and turned
  into a clean `parallel.spawn: could not start a worker thread` KiritoError (`dispatcher.hpp:587`),
  not a crash. (Not fork-bomb-tested live, to avoid rebooting the WSL box.)
- **Exception propagation** from a worker is caught in the parent as
  `parallel: worker thrown: <msg>` (`/tmp/exc.ki`).
- **WaitResult → error mapping all correct** (`/tmp/waitres.ki`): reentrant Lock, empty-queue timeout,
  graceful `close()` drains the buffered item *then* reports closed, full-queue `putnowait`, and a
  timed-out Barrier reports "broken".
- **Shared config (`registryMutex_`, `libPaths_`, `maxCallDepth_`, `gcThreshold_`) is guarded.**
  `addLibPath`/`setMaxCallDepth`/`setGcThreshold` write under the lock; `configureVM` snapshots under
  the lock and applies outside it (`stdlib_parallel.hpp:586`). No unguarded worker-visible read found;
  TSan saw none across all parallel probes.

### net
- **TLS verifies by default — CONFIRMED against a self-signed localhost server** (`openssl s_server`,
  `/tmp/tlsv.ki`): `verify=False` → 200; `verify=True` → rejected ("self-signed certificate");
  **default (no `verify` key) → rejected**, identical to `verify=True`. `verify` default is `true`
  (`stdlib_net.hpp:979`); handshake sets `SSL_VERIFY_PEER` + `SSL_set1_host` (hostname pinning)
  (`stdlib_net.hpp:678–691`).
- **Header / request-smuggling injection rejected** (`/tmp/url.ki`): CR/LF in a request header value,
  in a cookie, and in the URL/path are all rejected before any connect (`setHdr` `stdlib_net.hpp:942`,
  cookie check `:966`, `parseUrl` control-char reject `:322`, multipart field/filename `:902`).
- **URL parsing robust** (`/tmp/url.ki`): userinfo stripped at the *last* `@`, IPv6 literals bracketed,
  protocol-relative `//host`, out-of-range/non-numeric ports, and malformed `[::1` all handled with
  clear errors (no `std::stoi` leak, no port-vs-IPv6-colon confusion).
- **Redirect credential hygiene** (`redirectScope`, `stdlib_net.hpp:481`): Authorization dropped on
  host/port/scheme change (except same-host http→https std-port upgrade); cookie jar dropped on
  hostname change. Redirect count bounded (`maxredirects`, default 10). A non-http(s) `Location` cannot
  redirect off-protocol (resolved relative to the http origin, then re-`parseUrl`'d). Pure/unit-testable;
  logic confirmed by read (cross-origin live test not run — no second server).
- **Response size bounded** (256 MiB `kMaxRecvAll`) on both plain-TCP and TLS read loops
  (`stdlib_net.hpp:289`, `:737`) — a garrulous peer can't OOM the process.
- **Socket resource ownership** is RAII (`~SocketVal`/`closeFd` frees SSL + fd); `detach()` refuses a
  TLS socket and prevents double-detach/stale-fd leak.

### proc (`proc_compat.hpp` + `sys.createprocess`/`sys.shell`)
- **No shell injection via `createprocess`** — argv passed verbatim; `"hello; whoami"` is a single
  literal arg to `echo` (`/tmp/proc.ki`). `sys.shell` is the explicit opt-in to `/bin/sh -c`.
- **Poison-NUL truncation rejected** — an argument/cwd containing `\x00` throws
  ("must not contain a NUL byte", `stdlib_sys.hpp:50`) instead of silently truncating at the syscall
  boundary.
- **Spawn/cwd errors are clean & correctly attributed** — program-not-found vs bad-cwd reported
  distinctly (the exec-error pipe with a stage tag, `proc_compat.hpp:252`).
- **Timeout kills the WHOLE process group, no drain-hang, no orphans** — `sys.shell("sleep 100 & sleep
  100", timeout=1)` returns "process timed out" in **1.01s** (measured), and **zero** `sleep 100`
  processes survive (`setpgid(0,0)` in child + `kill(-pid, SIGKILL)`, `proc_compat.hpp:270,321`).
  No zombie: `waitpid` is always reaped.
- **stdout/stderr drained on separate threads** (deadlock-free for a child that fills a pipe);
  `ki-tsan` clean over repeated `createprocess` with 2000-line output (`/tmp/proctsan.ki`).
- **Output capped at 256 MiB** with a catchable "capture limit exceeded" (drain keeps reading to avoid
  child deadlock); `bad_alloc` in a drain thread is swallowed (can't `std::terminate`).
- **net+parallel combined**: 8 workers each doing `net.socketpair` send/recv concurrently
  (`/tmp/netpar.ki`): `ki-tsan` clean, deterministic.

---

## Honest nulls / limits of this pass
- No TSan race, deadlock, or livelock was reproduced anywhere in the parallel dispatcher despite
  targeted contention, teardown-of-blocked-workers, churn, and done-polling probes.
- The only real issue is F1 (no default HTTP timeout), which is a hardening/defaults call, not a
  memory-safety or race bug.
- Not exercised live: an intentional recursive-spawn fork bomb (unsafe to run here — bounded by OS
  limits + EAGAIN handling, verified by read); cross-origin redirect auth-strip against two live
  servers (logic verified by read + the CRLF-injection tests). Windows paths (`CreateProcessW`,
  Job-object tree-kill, Winsock socketpair emulation) were read but not executed (Linux-only WSL host;
  per policy no Windows-host access).
- Environment note: launching `openssl s_server` as a shell-backgrounded process in this sandbox
  reliably self-terminates with exit 144; the tool's own background mechanism worked and the TLS test
  above completed against it.
