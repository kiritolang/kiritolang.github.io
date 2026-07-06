# A15: net + parallel + dispatcher audit (v1.14)

Status: COMPLETE

Subsystem: `src/kirito/stdlib_net.hpp`, `net_compat.hpp`, `stdlib_parallel.hpp`, `dispatcher.hpp`.
Binary used: `/home/user/kiritolang.github.io/build-debug/ki` (parallel installed only by the CLI).
Legend: severity {info,low,medium,high}; confidence {low,med,high}.

Prior work honored: v1.13 A18 (CRLF injection, recv(0), detach/fileno, connectWithTimeout, dechunk
strtol, fromfd) and A22 (registry-never-erased, worker re-runs top level, negative argTimeout,
Barrier.abort permanence) NOT re-reported — confirmed still present where noted, hunted new angles.

## Findings

### A15-1: `listen()` on a closed socket bypasses `fdOrThrow` — leaks raw errno, breaking the "socket is closed" contract
- severity: medium
- location: `stdlib_net.hpp:1056-1062` (`if (::listen(sock(vm, self).fd, backlog) != 0)`)
- category: contract-violation / diagnostics
- description: EVERY other fd-touching Socket method routes through `fdOrThrow(op)` (connect/bind/
  accept/send/recv/recvall/sendto/recvfrom/shutdown/getsockname/getpeername/setsockopt/getsockopt/
  set*/setblocking/settimeout/detach — confirmed at lines 1035–1239). `listen` is the ONE exception:
  it reads `sock(vm, self).fd` directly, so on a closed/detached socket it calls `::listen(-1, ...)`
  → EBADF. CLAUDE.md promises "a use-after-close throws a clean '… : socket is closed' rather than a
  raw errno", and the in-repo test `verify_net.ki:284` comment even asserts "use-after-close (all ops
  share 'socket is closed')" — but listen is neither in that assert list nor honoring it.
- failure-scenario: CONFIRMED via probe — `s.close(); s.listen(5)` →
  `listen failed: Bad file descriptor` instead of `listen: socket is closed`.
- proposed-test: `assert raises_msg(Function(): return dead.listen(1), "socket is closed")` and the
  same for `accept()` (accept is actually correct — it uses fdOrThrow — but is untested).
- proposed-fix: `if (::listen(sock(vm, self).fdOrThrow("listen"), backlog) != 0)`.
- confidence: high

### A15-2: fire-and-forget spawn retains the Task (finished thread handle + result blob) for the whole process lifetime
- severity: low
- location: `dispatcher.hpp:563-593` (`taskDone` never erases; `joinTask` is the only eraser) +
  `stdlib_parallel.hpp` Task has no reaping path other than `join()`
- category: resource-leak
- description: A spawned Task is dropped from `tasks_` ONLY by `joinTask` (on `Task.join()`), or by
  `shutdown()` at teardown. A `Task` that is spawned and never joined (fire-and-forget, e.g.
  `discard parallel.spawn(work, i)`) stays in `tasks_` — holding a completed-but-unjoined
  `std::thread` object plus its serialized `result` blob — until the dispatcher is destroyed at
  process exit. `done()` observes completion but never reaps. A long-running server that spawns
  background work and relies on completion without joining grows memory monotonically. (Distinct from
  A22-1, which is about the sync-primitive maps `queues_/locks_/events_/...` + `waitables_` — those
  are ALSO still never erased in v1.14; confirmed.)
- failure-scenario: CONFIRMED benign at exit — 200 unjoined spawns run and the process still exits
  cleanly (shutdown reaps them). The leak is during-run retention, not a hang/crash.
- proposed-test: (hard to assert on memory) — at minimum a soak test spawning N>>cpucount tasks
  without joining and asserting termination; ideally expose a reaper so `done()==true` erases.
- proposed-fix: have `taskDone` (or a periodic sweep) reap+erase a Task once `done` is observed and
  no join is outstanding; or document that unjoined tasks are retained until shutdown.
- confidence: high (mechanism), med (practical impact — bundled servers always join)

### A15-3: HTTP redirect loop past `maxredirects` returns the final 3xx Response (ok=True) instead of raising
- severity: low
- location: `stdlib_net.hpp:917-938` (loop falls through to `return net::makeResponse(...)` when
  `redirect >= maxRedirects`)
- category: behavior / api-parity
- description: When the redirect cap is reached, netRequest returns the last redirect Response as-is.
  For a 302 that is `status < 400`, so `.ok` is `True` and `.raiseforstatus()` does NOT raise. Unlike
  `requests` (TooManyRedirects) a caller cannot tell a capped redirect chain from a real success
  except by inspecting `.status`.
- failure-scenario: CONFIRMED — a server returning `302 -> Location: /self` forever yields
  `redirloop status=302 ok=True` after 10 hops, silently.
- proposed-test: assert that a redirect loop returns status 302 (document) OR raises; whichever is
  the intended contract — currently unasserted either way.
- proposed-fix: either raise a clear "too many redirects" error, or document the return-the-3xx
  behavior explicitly (and consider making `.ok` false for an unfollowed redirect).
- confidence: high

### A15-4: `recv(n)`/`recvfrom(n)` pre-allocate min(n, 64 MiB) up front regardless of bytes available
- severity: info
- location: `stdlib_net.hpp:1084-1085` (`std::vector<char> buf(n)`), `1127-1128`
- category: resource / DoS-adjacent
- description: The receive buffer is sized to the requested `n` (capped at 64 MiB), not to what is
  pending. `recv(50000000)` transiently allocates ~50 MB even to read a 2-byte datagram. Bounded, so
  not a crash, but a loop doing large `recv(n)` calls churns large transient allocations.
- failure-scenario: CONFIRMED — `pair[1].recv(50000000)` returns `b'hi'` (len 2) after allocating a
  ~50 MB vector.
- proposed-test: n/a (perf/memory, not a correctness bug) — could assert recv of a huge n still
  returns the small payload (already implicitly works).
- proposed-fix: grow the buffer incrementally (e.g. cap the initial allocation to a smaller block and
  only expand toward n on a full read), matching a typical socket read chunk.
- confidence: high

### A15-5: `socketpair` on POSIX mislabels `.family` as "inet" though the socket is AF_UNIX
- severity: info
- location: `stdlib_net.hpp:1335-1336` (`SocketVal(fds[0], AF_INET, typ)`), pair is created via
  `::socketpair(AF_UNIX, ...)` in `net_compat.hpp:216`
- category: cosmetic / introspection
- description: The two SocketVals from `socketpair()` record `family = AF_INET`, so `.family` reports
  "inet" and `getsockname()` returns a meaningless `['localhost', 0]` for what is actually an unnamed
  AF_UNIX socket. Send/recv work fine; only introspection is wrong.
- failure-scenario: CONFIRMED — `pair[0].family == "inet"`, `pair[0].getsockname() == ['localhost', 0]`.
- proposed-test: assert socketpair `.family`/`.type` — currently only the transfer is tested.
- proposed-fix: record the real family (AF_UNIX on POSIX) or a synthetic "unix" label; or document
  that socketpair sockets carry a nominal family.
- confidence: high

### A15-6: worker exception TYPE is lost across `Task.join()` — a typed Kirito throw surfaces as a generic KiritoError string
- severity: low
- location: `dispatcher.hpp:713-716` (`errorText = worker.stringify(t.value)`), replayed at
  `stdlib_parallel.hpp:211` (`throw KiritoError("parallel: worker thrown: " + cached_)`)
- category: exceptions / api-parity
- description: When a spawned function throws a user exception, the worker stringifies the thrown
  value and the parent's `join()` re-throws it as a bare `KiritoError`. The original class is gone,
  so a parent `catch MyError as e` cannot match — only a bare `catch` can. CLAUDE.md/comments note
  typed exception classes are a future enrichment, so this is expected-but-worth-recording.
- failure-scenario: CONFIRMED — `throw "boom"` in a worker surfaces as
  `parallel: worker thrown: boom` (a String), catchable only by bare `catch`.
- proposed-test: assert a typed worker throw is caught by a bare `catch` and carries the message
  (documents the limitation).
- proposed-fix: serialize the thrown value (already serializable via `dump`) and rebuild+re-throw it
  in the parent so typed matching works.
- confidence: high

### A15-7: `setsockopt(option, value)` narrows int64 → int with no range check (silent truncation)
- severity: low
- location: `stdlib_net.hpp:1183` (`static_cast<int>(val)`); same at the boolSetter path
- category: validation
- description: The value is `asInt` (int64) then `static_cast<int>`. A value beyond INT_MAX wraps
  silently. `setsockopt("sndbuf", 999999999999)` became a wrapped value (OS then clamped to 8 MiB) —
  no error. Contrast with the careful 0–65535 port validation elsewhere.
- failure-scenario: CONFIRMED — no throw; effective sndbuf 8388608. A value like 4294967297 would
  become 1.
- proposed-test: assert setsockopt with a value > INT_MAX either throws or is documented as truncating.
- proposed-fix: range-check `val` to `[INT_MIN, INT_MAX]` and throw a clear error otherwise.
- confidence: high

## Confirmed-good (no finding — documents robustness)
- Use-after-close is clean for ALL methods EXCEPT listen (A15-1): recv/send/getsockname/getpeername/
  setsockopt/getsockopt/shutdown/settimeout/setblocking/detach all throw "op: socket is closed".
- detach→fileno(-1)→detach-again throws "detach: socket is closed"; no stale fd.
- Port validation (connect/bind/sendto: 0–65535) and URL parsing (scheme, IPv6 brackets, bad port,
  control chars → all clean errors).
- UDP: sendto/recvfrom, connect-then-plain-send, recvfrom returns [Bytes, [host, port]] — all work.
- HTTP: redirect loop caps; chunked-incomplete best-effort ("Hel" from a truncated 5-byte chunk);
  gzip-corrupt leaves raw content (no crash); gzip-ok inflates; 204 empty body; two Set-Cookie folded
  into the jar; raiseforstatus 4xx/5xx throws; header() is case-insensitive.
- setsockopt: wrong value type ("yes") → clean "expected Integer"; unknown option → clean list.
- parallel: spawn/join, join-twice-cached, done(); worker throw propagates; non-serializable ARG
  throws at spawn (packArgs); closure capturing an enclosing local → clean "name 'X' is not defined"
  in worker (locals don't cross, as documented); non-serializable RESULT throws on join; nested/
  recursive spawn works; forkbomb guard fires for a top-level spawn during bootstrap.
- Sync: Queue drain-before-closed, put/get on closed → "queue is closed", get/put timeouts, bounded
  queue full-timeout; Lock reentrant + double-release detected; Semaphore over-release grows permits;
  Barrier(1) immediate, Barrier(2) timeout → "barrier is broken".
- DEADLOCK-SAFETY: a worker blocked forever on `Queue.get()` with no join is aborted by the
  dispatcher's shutdown at process exit — CONFIRMED clean exit (code 0), no hang.
- Concurrency reasoning: Task result/hasError/errorText are non-atomic but only read after
  `reap()` joins the worker (happens-before); `done` is atomic; join()/shutdown() race resolved by
  `Task::joining.exchange`. Task is not serializable (no _getstate_) so it never crosses VMs — join
  is single-threaded per Task. No new race spotted (tsan is the gate).

## Coverage gaps (C++ `tools/tests/unit/*` + `.ki` scripts)
- **listen()/accept() use-after-close untested** — and listen is actually broken (A15-1). The
  verify_net.ki comment "all ops share 'socket is closed'" overclaims.
- **Redirect-loop cap behavior unasserted** (returns 3xx vs raises — A15-3).
- **Fire-and-forget Task retention untested** (A15-2) — every test/example joins.
- **setsockopt out-of-int-range value untested** (A15-7).
- **socketpair `.family`/`.type` introspection untested** (A15-5).
- **Worker typed-exception propagation untested** (A15-6) — only message-string catch is exercised.

## Summary
7 findings: 1 medium (A15-1 listen use-after-close contract violation), 4 low (A15-2 Task
retention, A15-3 redirect-loop returns-not-raises, A15-6 worker exception type lost, A15-7 setsockopt
int narrowing), 2 info (A15-4 recv buffer pre-alloc, A15-5 socketpair family mislabel).
Highest-value: A15-1 (a confirmed, one-line-fixable contract break that an in-repo test comment
falsely claims is covered). The subsystem is otherwise very robust — deadlock-safety, HTTP edge
handling, spawn isolation, and use-after-close (bar listen) all behave as documented.
