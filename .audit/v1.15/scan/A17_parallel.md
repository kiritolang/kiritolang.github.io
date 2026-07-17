# A17 Parallel/Dispatcher

Status: IN PROGRESS -> COMPLETE

Scope: `src/kirito/dispatcher.hpp`, `src/kirito/stdlib_parallel.hpp` (KiritoDispatcher, Waitable/
ConcurrentQueue/Lock/Event/Semaphore/Barrier, Task, spawn/join, findFunctionBySpan, configureVM incl.
gcThreshold propagation). Read-only on source/tests; this file is the only write.

## Read false-positives table first

Noted and will NOT re-flag: Semaphore over-release accepted, Lock release by non-owner accepted
(both intentional, documented in code comments too — `Lock::release` docstring explicitly calls out
the transferable-handoff design; `Semaphore::release` docstring explicitly calls out the
non-bounded-semaphore design).

## Provenance check

This subsystem has been audited repeatedly (pre-1.12 A19, v1.12 A19, v1.13 A22, v1.14 A15,
1.12.1). The code carries dense inline comments citing prior finding IDs (A08-2/A19-1/A19-2) for
fixes already landed: unbounded task-registry leak (fixed — `joinTask` erases after reap),
fork-bomb guard (`bootstrapping()` flag, cleared before the target fn runs so nested spawns work),
double-join safety (`joining` atomic + `reap()`), spawn-vs-shutdown race (`shuttingDown_` flag
checked in `spawnTask` under the registry lock), primitive-created-during-teardown pre-abort in
`makeWaitable`. I re-verified these by reading, not by assuming the comments are correct — see log
below. Re-flagging any of these as new findings would be a regression per the round's continuity
rule; they are not re-flagged.

## Empirical checks performed (ki debug binary, `.ki` scratch scripts, read-only on the repo)

1. Nested-spawn-from-inside-spawned-function works (bootstrapping flag correctly cleared before the
   target function runs) — confirmed via `parallel.Barrier` + two spawns rendezvousing.
2. `Barrier(3)` with only 2 arrivers + a 0.3s timeout: both waiters correctly get
   "barrier is broken" (`WaitResult::Broken` correctly threaded through both the timing-out waiter
   and its still-blocked peer via `brokenGen_`). No hang, no incorrect `Ok`.
3. Spawning a closure that captures NO enclosing-function locals (defined inside another function,
   not module top-level) works fine — spawn resolution is by (line,col) span, independent of where
   the value came from, and it works correctly as long as the body doesn't read a non-crossing local.
4. Spawning a closure that DOES read an enclosing function's local: confirmed it throws the
   documented `name 'x' is not defined` (the empty stand-in scopes work as designed) rather than
   silently reading garbage or crashing.
5. Top-level (unguarded) `parallel.spawn` correctly throws the fork-bomb diagnostic when a worker
   re-loads the file during bootstrap (confirmed by accident while writing test scripts — forgetting
   `if argmain:` immediately reproduces the intended guard, not a hang).

## Findings

### A17-1: Negative `timeout` on every parallel primitive silently behaves as a non-blocking check instead of a clear error  [LOW] [HIGH]
- **Location**: `stdlib_parallel.hpp:52-59` (`argTimeout`), consumed by `Lock.acquire`/`Semaphore.acquire`/
  `Event.wait`/`Barrier.wait`/`Queue.put`/`Queue.get` via `dispatcher.hpp`'s `waitDuration`/
  `cv.wait_for`.
- **What**: `argTimeout` accepts any Integer/Float, including negative, with no range check. A
  negative duration passed to `std::condition_variable::wait_for` returns immediately (already
  "timed out"), so `timeout = -1` behaves exactly like `timeout = 0` (an instant poll) rather than
  raising a usage error. Every other numeric "must be sane" input in this module (e.g. `Queue`
  `maxsize < 0`, `Semaphore` `value < 0`, `Barrier` `parties < 1`) is explicitly validated and
  throws; timeout is the one bound left unchecked. This was flagged before (v1.13 A22-3) but never
  landed a fix or an explicit "reject" verdict in any round's FINDINGS.md — it is still live.
- **Repro** (confirmed live against `build-debug/ki`):
  ```
  var parallel = import("parallel")
  var q = parallel.Queue()
  q.get(True, -1)   # throws "get from an empty queue" instead of blocking or rejecting -1
  var s = parallel.Semaphore(0)
  s.acquire(True, -1)   # returns False (silently "timed out") instead of throwing
  ```
- **Proposed fix**: in `argTimeout`, throw `KiritoError("parallel: timeout must be >= 0")` when the
  parsed value is negative (mirroring the `maxsize`/`value`/`parties` validation already present a
  few lines away in the same file).
- **Proposed test**: a `.ki` case per primitive (`Queue.get/put`, `Lock.acquire`, `Semaphore.acquire`,
  `Event.wait`, `Barrier.wait`) asserting `timeout = -1` raises a clear "timeout must be" error
  rather than returning `False`/throwing the generic empty/full-queue message.

### A17-2: `ConcurrentQueue::get()` prioritizes draining a buffered item over reporting `Aborted`, unlike every other primitive (which prioritizes `Aborted`)  [LOW] [MEDIUM]
- **Location**: `dispatcher.hpp:81-93` (`ConcurrentQueue::get`) vs. `Lock::acquire` (140-153),
  `Event::wait` (191-200), `Semaphore::acquire` (220-231), `Barrier::wait` (261-292) — all of which
  check `if (aborted_) return Aborted;` **before** doing their "success" bookkeeping.
- **What**: `get()`'s ready-predicate is `aborted_ || closed_ || !q_.empty()`; once ready, it checks
  `!q_.empty()` FIRST and returns the buffered item (`Ok`) even if `aborted_` is also true, only
  falling through to `Aborted`/`Closed` once the queue is drained. The inline comment ("always drain
  available items before reporting closed") only reasons about the `close()` case (a producer done
  writing, consumer should still get what's buffered) — it does not appear to have considered that
  `abort()` (fired by `KiritoDispatcher::shutdown()`) hits the exact same code path. Net effect: a
  worker blocked in `Queue.get()` during dispatcher shutdown, if items are still buffered, keeps
  running ordinary Kirito code (processing those items) instead of unwinding immediately — the
  "every blocked worker unwinds on abort" invariant documented at the top of `dispatcher.hpp` is true
  only up to the depth of the queue's backlog, not instantly. Not a deadlock (it still terminates
  once the queue drains and the next `get()` sees `Aborted`), but it is an inconsistency with the
  Lock/Event/Semaphore/Barrier ordering and a possible surprise if a caller assumes shutdown means
  "stop immediately, do not process more application data."
- **Repro** (reasoned from code, not yet run under a live shutdown race — flagging as a coverage
  gap too, see A17-4): put 3 items into a `Queue`, then call dispatcher `shutdown()` from another
  thread while a worker calls `get()` 4 times in a loop — expect the first 3 to return `Ok` with the
  buffered values and only the 4th to return the `Aborted`/"operation aborted" error, even though
  `shutdown()` had already fired before any of the 4 calls ran.
- **Proposed fix**: N/A as a *bug* fix (the drain-before-close behavior for graceful `close()` is
  probably intentional and shouldn't change) — if the divergence from abort-priority is undesired,
  the minimal change is checking `aborted_` before draining, i.e. `if (aborted_) return Aborted; if
  (!q_.empty()) {...}; if (closed_) return Closed;`. Recommend confirming intent before touching this
  — could easily be by-design (a worker should still get to finish an in-flight batch rather than
  losing already-produced data on shutdown) and get added to the false-positives table instead.
- **Proposed test**: whichever way it's resolved, pin the chosen behavior with a C++ test that fills
  a queue, aborts it, and asserts what a subsequent `get()` returns.

### A17-3: `spawnTask`'s `std::thread` construction failure leaves an orphaned, unreachable `Task` in the registry (leak, and no clean error to the caller)  [LOW] [LOW-MEDIUM, latent/hard to trigger]
- **Location**: `dispatcher.hpp:563-576` (`spawnTask`).
- **What**: the sequence is: allocate `Task`, insert it into `tasks_[id]` under the registry lock,
  THEN construct `tp->thread = std::thread(...)`. If thread construction throws (`std::system_error`,
  e.g. `EAGAIN` from `pthread_create` under thread/resource exhaustion — a realistic failure mode for
  a long-running server spawning many short workers), the exception propagates out of `spawnTask`
  (the `lock_guard` unwinds safely, no deadlock), but the `Task` entry already inserted into
  `tasks_` is never referenced by any `TaskVal` (the `spawn()` builtin never gets to construct one),
  so it can never be `join()`ed or erased — a permanent, silent per-failed-spawn leak of one `Task`
  (empty result string, small but nonzero) for the life of the dispatcher. Additionally, since the
  `Task`'s `thread` member was never actually assigned (assignment-from-throwing-temporary never
  completes), `t->thread.joinable()` is `false`, so `shutdown()`'s `reap()` on this orphan is a
  harmless no-op — it does NOT hang shutdown, it just never disappears from `tasks_` (until process
  exit). The `std::system_error` itself crosses the native-function boundary as a non-`KiritoError`
  C++ exception; whether the surrounding native-call harness converts that into a catchable Kirito
  error was not traced in this file (out of my two files) — worth a cross-check by whichever
  agent owns the native-call/dispatch boundary.
- **Repro**: hard to trigger deterministically in a portable test (needs thread-creation exhaustion,
  e.g. `ulimit -u` very low, or thousands of concurrent un-joined spawns). Flagging as latent/
  theoretical rather than demonstrated.
- **Proposed fix**: wrap the `std::thread` construction in a try/catch inside `spawnTask`; on failure,
  erase the just-inserted `tasks_[id]` entry and rethrow as a `KiritoError("parallel.spawn: failed to
  start worker thread: ...")` so the caller gets a clean, catchable diagnostic and the registry
  doesn't leak the orphan.
- **Proposed test**: a C++ unit test that injects a thread-creation failure (or simply asserts, via
  code inspection / a mock, that the map entry is removed on the throwing path) — genuine
  fault-injection here is disproportionate effort for a LOW-severity leak; note as accepted risk if
  not pursued.

## Coverage gaps (enumerated primitive x condition matrix)

Existing coverage is broad (`test_parallel.cpp`, `test_parallel_sync.cpp`, `test_parallel_deadlock.cpp`,
`test_parallel_deep.cpp`, `test_parallel_edge.cpp`, `test_parallel_fuzz.cpp`, `test_parallel_random.cpp`,
plus a dozen `.ki` golden scripts incl. `parallel_forkbomb_guard.ki`, `probe_parallel_adversarial.ki`,
`probe_parallel_soak.ki`). Gaps found:

- **COVERAGE GAP: `KiritoDispatcher::setGcThreshold` propagation to WORKER VMs is untested.** Grep of
  every `test_*.cpp`/`.ki` under `tools/tests` turns up plenty of `vm.setGcThreshold(...)` calls on a
  bare `KiritoVM`, but none that go through `KiritoDispatcher::setGcThreshold` + `spawn` to confirm a
  worker VM actually inherits the pinned threshold (vs. its own adaptive default). This is called out
  explicitly in the audit brief as a NEW propagation path (`configureVM` snapshotting
  `libPaths_`/`maxCallDepth_`/`gcThreshold_` together under one lock — read correctly, see below).
  Proposed test: spawn a worker that allocates heavily and reports `vm` GC stats (e.g. via a
  C++-level hook, since Kirito itself has no `gcThreshold()` getter exposed to script) after the
  dispatcher had `setGcThreshold(1)` called on it before spawning — or, more simply, a C++
  `test_parallel_*.cpp` addition that spawns a task and checks the worker VM's `gcThreshold_`
  indirectly via observed collection frequency.
- I did verify by reading that the snapshot-then-apply pattern in `configureVM` (`stdlib_parallel.hpp:
  572-584`) takes `libPaths_`, `maxCallDepth_`, AND `gcThreshold_` under **one** `registryMutex_`
  critical section before releasing it and applying all three to the worker VM outside the lock —
  so there is no torn read between the three settings, and no race with `addLibPath`/
  `setMaxCallDepth`/`setGcThreshold` being called concurrently on another thread while a worker is
  spawning. This is correct as written; only the *test* coverage is missing.
- **COVERAGE GAP: `socket.detach()` / `net.fromfd()` handed across the `parallel` spawn boundary is
  only exercised end-to-end via the Python harnesses for `sqldb`/`webserver`/`sqldb_kwargs`/
  `webserver_kwargs`** (`examples/big_projects/*/test_client.py`, `test_concurrent.py`), not by any
  focused unit/golden `.ki` test that isolates "accept a connection in VM A, detach its fd, spawn a
  worker with the fd as a plain Integer argument, `net.fromfd` it there, and confirm both ends still
  work" without the extra surface area of a full HTTP/SQL server. A minimal `parallel_socket_handoff.ki`
  would pin the ownership-transfer contract (my subsystem's half of it — the actual fd/TLS semantics
  are `net`'s territory, so this is a cross-cutting gap, not something to fix in my two files).
- **COVERAGE GAP: a `Queue` in a per-primitive x per-condition matrix.** Existing scripts cover
  timeout/nowait/close/shutdown-abort reasonably well for `Queue`, but I did not find a test that
  specifically fills a `Queue` then races `abort()`/`shutdown()` against a `get()` loop to pin down
  the drain-vs-abort ordering in A17-2 above (whichever way it's resolved).
- **COVERAGE GAP: Barrier concurrent multi-timeout interleavings.** `Barrier` correctness under a
  single timing-out waiter is tested (confirmed live above); a barrier with parties > 2 where TWO
  waiters time out in the same generation concurrently (exercising the `brokenGen_ != gen` guard's
  "only break once" logic under a genuine data race, not just sequential code) did not turn up in a
  targeted grep of `test_parallel_sync.cpp`/`test_parallel_deadlock.cpp` — worth a targeted addition
  since that guard is exactly the kind of logic that looks right sequentially but hides a race.
- **COVERAGE GAP: spawning a value that fails to serialize as an argument** (e.g. `parallel.spawn(fn,
  someTask)` — passing a live `Task` as an argument to another spawn; `TaskVal` has no `_getstate_`/
  `_setstate_`, unlike Queue/Lock/Event/Semaphore/Barrier, so `packArgs`'s `dumpfmt::write` should
  throw a clear "not serializable" error). Not found in the golden scripts; worth one assertion to
  pin that a `Task` is intentionally NOT one of the cross-VM-by-identity primitives.
- **COVERAGE GAP: `spawnTask` racing `shutdown()` at the exact `shuttingDown_` check boundary** is
  logically covered by `test_parallel_deadlock.cpp` per the file name, but I did not exhaustively
  read that file line-by-line to confirm it specifically targets the `spawnTask` throw path (vs. only
  the wait-primitive abort path) — flagging as "verify, not confirmed absent" rather than a hard gap.

## DRY notes (no functional impact)

- The `_getstate_`/`_setstate_` boilerplate is identical in shape across `QueueVal`/`LockVal`/
  `EventVal`/`SemaphoreVal`/`BarrierVal` (five near-identical pairs differing only in the C++ type and
  the `dNNNById` lookup call) — same DRY observation as v1.13 A22-14, still true, still low-priority
  (five call sites, each three lines, a shared template helper would need a type tag or trait per
  primitive; not clearly worth the indirection).
- The blocking-wait idiom (`ready` predicate + `cv.wait_for`/`cv.wait` + aborted-check) is
  hand-duplicated across `Lock::acquire`/`Event::wait`/`Semaphore::acquire`/`Barrier::wait`; only
  `Queue` factored it into `awaitPred`. Same as v1.13 A22-13. Given each of the four has slightly
  different post-wait bookkeeping (Lock sets `held_`/`owner_`, Semaphore decrements, Barrier tracks
  generation/index), a shared helper would only extract the wait itself, saving ~3 lines per site —
  marginal, not flagged as an action item.

## Summary

Read `dispatcher.hpp` (771 lines) and `stdlib_parallel.hpp` (592 lines) in full, cross-referenced
against four prior audit rounds' findings for this subsystem (pre-1.12, v1.12, v1.13, v1.14) to avoid
re-litigating already-fixed issues, and empirically verified five behaviors live against the debug
`ki` binary (nested-spawn-from-worker, Barrier partial-arrival + timeout, non-crossing enclosing
locals throwing "not defined", crossing-safe closures working, fork-bomb guard firing). The core
design (share-nothing VMs, Waitable/abort/shutdown, strict single-join via the `joining` atomic,
`shuttingDown_`-gated spawn, the NEW `gcThreshold_` propagated alongside `libPaths_`/`maxCallDepth_`
under one lock in `configureVM`) is sound and matches its extensive documentation. No data race,
deadlock, or use-after-free was found in this pass. Three findings, all LOW severity: (1) negative
`timeout` silently treated as non-blocking instead of rejected, confirmed live and reproducible; (2) a
`Queue.get()` drain-vs-abort ordering inconsistency with the other four primitives, worth a design
decision + pinning test either way; (3) a latent, hard-to-trigger `Task` registry leak on
`std::thread` construction failure. Plus five enumerated coverage gaps, the most actionable being the
untested `gcThreshold` worker-propagation path (a genuinely new mechanism this round) and the missing
`socket.detach`/`net.fromfd`-through-`spawn` focused test (currently only exercised indirectly via
the big-project Python harnesses).
