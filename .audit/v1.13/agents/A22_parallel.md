# A22 — parallel + dispatcher audit (v1.13)

Scope: `src/kirito/stdlib_parallel.hpp`, `src/kirito/dispatcher.hpp` — the ONLY concurrent code
(true multiprocessing: N share-nothing KiritoVMs, one per OS thread, communicating via serialized
blobs through thread-safe primitives owned by `KiritoDispatcher`). This is the tsan gate area.
READ-ONLY audit; no source/test modified; reasoned statically (no build/run).

Method: read CLAUDE.md parallel section, then both sources in full, then the unit tests
(`test_parallel*.cpp`) and `.ki` probes (`probe_parallel*`, `audit_parallel`, `r*_parallel`,
`parallel_*`).

## Bottom line
The concurrency core is unusually careful and I found **no confirmed data race, lost-wakeup, or
lock-order inversion** — predicate-based waiting + notify-on-every-transition + a single lock ordering
(registryMutex_ → primitive m_, never the reverse) + a correct atomic release/acquire on `Task::done`
all hold up under scrutiny. The real defects are two **resource / footgun** issues (unbounded primitive
registry growth; top-level side-effects re-execute in every worker) plus a set of coverage gaps and
DRY cleanups. Detail below; confirmed defects first, then gaps, then DRY.

---

### A22-1: Sync-primitive registry grows unbounded (memory leak) — primitives never erased
- severity: medium
- location: `dispatcher.hpp` — `makeWaitable` (`queues_/locks_/events_/semaphores_/barriers_` + `waitables_`), no counterpart erase anywhere; contrast `joinTask` which *does* erase `tasks_`.
- category: resource leak / unbounded growth
- description: Every `parallel.Queue()/Lock()/Event()/Semaphore()/Barrier()` inserts a `shared_ptr` into its map (kept alive for the dispatcher's entire life) and a `weak_ptr` into `waitables_`. Nothing ever removes them. The `shared_ptr` in the map keeps the C++ primitive alive even after every Kirito reference is GC'd, so the object cannot be reclaimed. The task path was explicitly fixed to erase on join (comment cites A08-2/A19-2); the primitive path was not.
- failure-scenario: A long-running server that creates a fresh `Queue`/`Lock` per request/connection (the exact spawn-per-request server model CLAUDE.md calls out) leaks one primitive + one dead `weak_ptr` per request forever. `shutdown()` also re-locks and iterates the whole `waitables_` vector, which only grows. Steady memory climb, unbounded.
- proposed-test: create N=100k queues on a dispatcher, drop all Kirito refs (force GC), assert the C++ side memory / map size does not grow without bound; or a soak that creates+discards a per-iteration Lock for M iterations and checks RSS stays flat.
- proposed-fix: reference-count the wrapper's binding, or reap primitives whose last external `shared_ptr` is gone (the map could hold `weak_ptr` and `byId` upgrade), pruning dead `waitables_`/map entries. At minimum document that primitives are per-dispatcher-lifetime and must be created at startup, not per request.
- confidence: high (that entries are never erased); medium (on whether any shipped example actually creates per-request primitives — the big-project servers appear to use a fixed startup set).

### A22-2: Worker re-executes the ENTIRE source top level — non-`spawn` side-effects run once per spawn
- severity: medium
- location: `dispatcher.hpp` `runWorker` — `worker.evalIn(src, modScope, sourceFile)` re-evaluates the whole file top-to-bottom to rebuild the spawned closure's environment; `stdlib_parallel.hpp` spawn guard only blocks `spawn` itself while `bootstrapping()`.
- category: correctness footgun / semantics
- description: To resurrect a spawned function's closure, each worker re-runs the file's entire module top level. Only a top-level `spawn` is rejected (fork-bomb guard). Every OTHER top-level side effect — `io.print`, file writes, `net` calls, mutating a top-level structure, `sys.setenv`, opening a socket — executes afresh in every worker VM. The only defense is the user wrapping side effects in `if argmain:` (False in workers). The diagnostic and the CLAUDE.md text emphasize guarding `spawn`, under-selling that ALL side-effecting top-level code must be guarded.
- failure-scenario: A file with an unguarded top-level `io.print("starting")` or a top-level counter file-write prints/writes once per spawned worker; a top-level `net.get(...)` fires N extra requests. Surprising and easy to hit.
- proposed-test: spawn a worker from a file whose top level appends a line to a temp file (not under `if argmain:`); assert the file gains an extra line per spawn — documents the behavior and guards against a future change that would silence it.
- proposed-fix: mainly a docs/diagnostic fix — state plainly that a spawnable file must keep ALL side effects behind `if argmain:`, since a worker re-evaluates the module body. (A deeper fix — snapshotting the closure env instead of re-evaluating — is out of scope.)
- confidence: high (behavior is exactly as described in code).

### A22-3: `argTimeout` accepts a negative timeout (silent nowait), unlike other bounds which throw
- severity: low
- location: `stdlib_parallel.hpp` `argTimeout` (lines ~52–59) → feeds `waitDuration(seconds)` in every primitive.
- category: input validation inconsistency
- description: `maxsize < 0`, `Semaphore value < 0`, `Barrier parties < 1` all throw a clear error, but a negative `timeout` (e.g. `q.get(True, -5)`) passes straight through to `std::condition_variable::wait_for` with a negative duration, which returns immediately — silently degrading a blocking call to nowait. Inconsistent with the module's otherwise strict validation.
- failure-scenario: `lock.acquire(True, -1)` returns `False` instantly instead of blocking; a user typo turns a blocking wait into a busy fail.
- proposed-test: `expectErr`-style: `q.get(True, -1)` should throw "timeout must be >= 0" (once fixed); today it silently times out.
- proposed-fix: reject a negative timeout in `argTimeout` with a clear message.
- confidence: high.

### A22-4: `Barrier.abort()` is exposed to Kirito and permanently, irrecoverably kills the barrier for ALL VMs
- severity: low
- location: `stdlib_parallel.hpp` BarrierVal `abort` binding → `Barrier::abort()` sets `aborted_ = true` with no reset path (unlike `resetBarrier` which is recoverable).
- category: API sharp-edge (not a bug — by design, but a foot-gun worth a gap note)
- description: `Barrier.abort()` uses the SAME `aborted_` flag that `shutdown()` uses. Once any VM calls it, every present and future `wait()` on that barrier (across every worker sharing it by identity) returns `Aborted` forever; there is no un-abort. `reset()` (recoverable break) is the intended re-arm; `abort()` is a one-way kill. Easy to confuse `reset` vs `abort`.
- failure-scenario: a worker calls `bar.abort()` intending a soft reset; all peers now permanently get "operation aborted (dispatcher shut down)" — a misleading message, since the dispatcher is NOT shutting down.
- proposed-test: `bar.abort()` then `bar.wait()` throws; assert the message and that it is permanent; confirm `reset()` (not `abort`) is the re-arm.
- proposed-fix: at least give a barrier-abort a distinct message (not "dispatcher shut down"); consider whether exposing the permanent `abort` to Kirito at all is desirable vs only `reset`.
- confidence: high (behavior); the "should it exist" is a design call.

### A22-5: `net.socket.detach()` / `net.fromfd()` handoff not covered here / cross-check needed
- severity: info / gap
- location: not in these two files (lives in `net`), but part of A22's mandated surface (the accepted-connection handoff to a worker VM).
- category: coverage boundary
- description: The parallel spec includes `socket.detach()`/`net.fromfd(fd)` to hand an accepted fd to a worker VM in the same process. This audit's two files do not implement it; it belongs to the net audit. Flagging so it is not assumed covered. The dispatcher/parallel side merely transports the integer fd (a plain Integer through a Queue/spawn arg), so there is no parallel-specific race here — but the fd's dup/ownership semantics (double-close if both VMs close the same fd) should be audited on the net side.
- proposed-test: (net area) two VMs, one detaches an accepted socket fd, worker adopts via `fromfd`, both proceed; assert no double-close/EBADF.
- confidence: n/a (out of these files).

---

## Coverage gaps (tested elsewhere? logged comprehensively)

### A22-6: No test exercises the shutdown-vs-spawn race (`shuttingDown_` guard in `spawnTask`)
- severity: gap
- location: `dispatcher.hpp` `spawnTask` (throws "the dispatcher is shutting down") and `makeWaitable`'s pre-abort of primitives born during teardown.
- description: The most delicate teardown invariants — (a) a `spawn` issued after `shutdown()` began must be refused (else an un-joined thread dereferences a freed Task), and (b) a primitive created by a worker in the window between shutdown's abort loop and its join loop is pre-aborted so a subsequent wait can't hang the join — have no dedicated test. They are inherently timing-dependent, but a stress loop (spawn/create in a tight loop from a worker while the main thread calls shutdown) would give TSan/the watchdog a chance to catch a regression.
- proposed-test: a worker that loops `d.createQueue()`/nested-spawn while main races `shutdown()`; assert clean teardown within the watchdog budget, no hang, no crash under tsan.
- confidence: high that no such test exists (grep of the suite shows only clean-teardown-after-idle cases).

### A22-7: No test for a worker returning a NON-serializable result (result-side dump failure)
- severity: gap
- location: `dispatcher.hpp` `runWorker` — `tp->result = dumpfmt::write(worker, r)` "throws if the result isn't serializable"; caught → `hasError`.
- description: The adversarial probe covers a worker RETURNING a `net.Socket()` (`w_return_socket`) — good, that DOES exercise result-side non-serializability. So this is partially covered. But other non-serializable results (an open File/BytesIO, a compiled Regex, a live Session) aren't individually exercised; the code path is shared, so risk is low. Log as a minor completeness gap only.
- proposed-test: parametrize `w_return_socket` over File/Regex/Session results; assert `join()` throws a clear catchable error and the host survives.
- confidence: medium (the socket case already covers the shared path).

### A22-8: TOCTOU on `qsize()/empty()/full()` is inherent and untested-as-a-caveat
- severity: gap (documentation)
- location: `dispatcher.hpp` `ConcurrentQueue::size/empty/full`; `stdlib_parallel.hpp` bindings.
- description: `if not q.empty(): q.getnowait()` from two consumers races — one gets `getnowait empty throws`. This is expected MPMC behavior (the snapshot is stale the instant it returns), but there is no test/doc asserting that callers must handle `getnowait`/`putnowait` throwing rather than trust `empty()`/`full()`. The edge test covers `getnowait empty throws` in isolation but not the racing pattern.
- proposed-test: two workers both `if not q.empty(): getnowait()` on a 1-item queue; assert exactly one succeeds and the other's throw is catchable (never a crash).
- confidence: high.

### A22-9: `reap()` spin-wait path (`while(!done) yield()`) is a busy-wait; no test forces the join()/shutdown() race that reaches it
- severity: gap / minor perf
- location: `dispatcher.hpp` `reap` — the `else` branch when another caller already claimed `joining`.
- description: When `join()` (from Kirito) and `shutdown()` race on the same Task, the loser spins `std::this_thread::yield()` until `done`. Correctness is fine (documented, and `done` is atomic with proper release/acquire), but (a) it is a busy spin, and (b) no test deterministically drives both callers onto the same Task to exercise the branch. Since a Task is owned by exactly one VM (TaskVal has no `_getstate_`/`_setstate_`, so it can't cross VMs), the only way to reach the else-branch is `shutdown()` (dispatcher dtor) racing a live `join()` — real, but untested.
- proposed-test: spawn a slow worker, from the owning VM call `join()` while another thread calls `shutdown()`; run under the watchdog; assert one clean result and no double-join UB.
- proposed-fix (optional): replace the yield-spin with a condvar on `done` to avoid burning a core if the join is long.
- confidence: high.

### A22-10: No explicit test that a spawned fn's closure captures ENCLOSING-FUNCTION locals do NOT cross (only file/module scope survives)
- severity: gap
- location: `runWorker` rebuilds the closure from `modScope` only (re-evaluated module top level) + the located `FunctionExpr`.
- description: CLAUDE.md states "locals captured from an enclosing function do NOT cross". The adversarial probe's `leaky` case (worker has no `secret` → NameError) covers the negative — good. Confirming this is exercised; leaving as covered. (Recording that I verified the caveat is tested, not a new gap.)
- confidence: high (covered by `probe_parallel_adversarial`'s `leaky`).

### A22-11: Barrier concurrent-timeout / broken-generation interleavings only lightly tested
- severity: gap
- location: `dispatcher.hpp` `Barrier::wait` timeout branch + the "last arrival on an already-broken generation returns Broken" path (lines ~266–291).
- description: The logic here is the most intricate (a timed-out waiter fully resets the generation; the `brokenGen_` guard makes concurrent timeouts break only once; the last arrival must report Broken if its generation was pre-broken). The unit tests cover `Barrier.reset re-arm reuse` and a single-party `wait(0.05) == Broken`, but not: N-party partial arrival where one waiter times out and the rest must all get Broken while a subsequent fresh cohort succeeds cleanly; nor two waiters timing out in the same generation simultaneously; nor the "last arrival lands exactly as the generation was broken" race.
- failure-scenario: a subtle regression in the reset bookkeeping (e.g. forgetting `++generation_` on the timeout path) would leave `count_` polluted and trip a spurious "last party" for a later cohort — deadlock or wrong index. Not caught today.
- proposed-test: parties=3, two arrive, one times out; assert both blocked peers return Broken; then a fresh 3 arrive and rendezvous Ok with correct 0/1/2 indices. Plus a race variant under the watchdog with concurrent timeouts.
- confidence: high.

### A22-12: `spawnTask` id-space / `nextId_` monotonic; no overflow guard (theoretical)
- severity: info
- location: `dispatcher.hpp` `nextId_` (uint64, single id space for tasks + all primitives).
- description: `++nextId_` never wraps in practice (2^64). Non-issue; logged for completeness only. If it ever wrapped, `byId` could collide a reused id with a live primitive.
- confidence: high (non-issue).

---

## DRY / structural

### A22-13: The blocking-wait idiom is duplicated across Lock/Event/Semaphore; only Queue factored it
- severity: cleanup
- location: `dispatcher.hpp` — `ConcurrentQueue::awaitPred` is a clean helper, but `Lock::acquire`, `Event::wait`, `Semaphore::acquire` each re-inline the identical `if(!ready()){ if(!block) …; if(timeout) wait_for else wait; }` shape, and each re-declares `mutable std::mutex m_; std::condition_variable cv_; bool aborted_=false;` plus an identical `abort(){ {lock; aborted_=true;} cv_.notify_all(); }`.
- category: DRY
- description: Five Waitables repeat the same mutex/cv/aborted_ scaffolding and the same wait-with-optional-timeout-and-abort dance. A shared `WaitableBase` (holding `m_/cv_/aborted_` and providing `awaitPred` + a default `abort()`) would remove ~5 copies and make the abort/notify contract single-sourced (reducing the chance a future primitive forgets `notify_all` on abort).
- proposed-fix: extract `WaitableBase` with the mutex/cv/aborted_ and a protected `awaitPred`/`abort`; have each primitive derive and supply only its predicate/state.
- confidence: high.

### A22-14: `_getstate_`/`_setstate_` binding boilerplate duplicated across all five *Val wrappers
- severity: cleanup
- location: `stdlib_parallel.hpp` — QueueVal/LockVal/EventVal/SemaphoreVal/BarrierVal each hand-roll near-identical `_getstate_` (emit `id()` as Integer) and `_setstate_` (require(1) → requireDispatcher → `xById` → null-check → assign), plus the `bind`/`que|lk|ev|sm|br` accessor lambda and the "uninitialized" guard.
- category: DRY
- description: Five copies of the same cross-VM-identity dance (getstate=id, setstate=rebind-by-id-or-throw) and the accessor+null-guard lambda. A small CRTP/template helper parametrized on (dispatcher lookup fn, member pointer, type name) would collapse it and guarantee the five stay in lockstep (a future sixth primitive can't forget one arm).
- proposed-fix: a `bindIdentity<T>(...)` helper generating both dunders + the accessor from the dispatcher's `xById` and the wrapper's `shared_ptr` member.
- confidence: high.

---

## Things I checked and found CORRECT (no finding — recorded so the negative space is explicit)
- **No lost wakeup:** all primitives use predicate-based `wait`/`wait_for` over persistent STATE and notify on every transition (put/get-pop/release/close/set/abort). A notified-then-timing-out waiter re-checks the predicate under the lock, so it can only "consume" a notification by actually taking the state; otherwise the state persists for the next acquirer, which is always itself notified on the next transition. Queue/Lock/Semaphore/Event/Barrier all hold.
- **No lock-order inversion:** the only nested locking is `registryMutex_` → a primitive's `m_` (in `makeWaitable`'s teardown pre-abort). No path ever takes a primitive `m_` then `registryMutex_`. `shutdown()` deliberately releases `registryMutex_` before calling `abort()`/`reap()`. `addLibPath`/`setMaxCallDepth`/`configureVM` never hold `registryMutex_` across a VM call or thread join (comments confirm and code matches).
- **`Task::result`/`errorText` visibility:** worker writes them then `done.store(true)` (seq_cst = release); the reaper either `thread.join()`s (full sync) or `done.load()`s true (acquire), establishing happens-before for the non-atomic reads. Correct.
- **join()/shutdown() double-join safety:** the `joining.exchange(true)` claim + `shared_ptr` copies (both `joinTask` and `shutdown` hold their own) prevent two `std::thread::join()`s and prevent erase-from-under. Correct.
- **TaskVal cannot cross VMs** (no `_getstate_`/`_setstate_`), so exactly one VM/thread ever joins a given task — the `join()` idempotence cache needs no locking, as the comment claims. Correct.
- **Queue drains before reporting Closed/Aborted** (items delivered even after close/abort); unbounded when `maxsize==0`; bounded back-pressure correct. Correct.
- **Fork-bomb guard** (`bootstrapping()` blocks top-level spawn) with correct clear-before-target-call so nested spawns from inside the spawned fn still work (A19-1). Correct.

---

## Summary
- Confirmed defects: **4** (A22-1 unbounded primitive registry leak [medium], A22-2 top-level re-exec side-effect footgun [medium], A22-3 negative-timeout silent nowait [low], A22-4 permanent `Barrier.abort` w/ misleading message [low]).
- Gaps: **7** (shutdown-vs-spawn race untested; result-side non-serializable [partial]; qsize/empty/full TOCTOU caveat; reap spin-wait race untested + busy-spin; barrier concurrent-timeout interleavings; plus info-level id overflow and a net-handoff boundary note).
- DRY: **2** (WaitableBase extraction; identity-dunder helper).
- Verified-correct (no finding): no lost wakeup, no lock-order inversion, correct atomic release/acquire on Task result, safe double-join, correct queue drain/back-pressure, correct fork-bomb guard.

### Top 5
1. **A22-1** — sync primitives (Queue/Lock/Event/Semaphore/Barrier) are never erased from the dispatcher maps; per-request creation leaks unboundedly for the dispatcher's whole life (contrast the task path, which was fixed to erase on join).
2. **A22-2** — a worker re-evaluates the entire module top level to rebuild the closure, so ALL unguarded top-level side effects (print/file/net/mutation) re-run once per spawn; only `spawn` itself is guarded. Docs/diagnostic under-warn.
3. **A22-11 / A22-6** — the trickiest concurrency paths (Barrier broken-generation/concurrent-timeout interleavings; shutdown-vs-spawn / born-during-teardown pre-abort) have no dedicated race test.
4. **A22-3** — negative `timeout` silently degrades a blocking op to nowait, inconsistent with the module's otherwise-strict bounds validation.
5. **A22-13 / A22-14** — heavy duplication: the mutex/cv/aborted_/wait scaffolding across five Waitables, and the `_getstate_`/`_setstate_` identity boilerplate across five *Val wrappers, should be factored to keep the abort/notify and cross-VM-identity contracts single-sourced.
