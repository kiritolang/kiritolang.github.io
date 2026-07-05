# parallel — doc-vs-impl verification findings

Test: `tools/tests/scripts/verify_parallel.ki` (+ `.expected` = `OK parallel\n`), 83 assertions.
Run through `build-debug/ki` (the CLI provides the `KiritoDispatcher`; a bare embedded VM has no
`parallel` module). Deterministic: cross-thread facts asserted only on aggregates (sum/set/sorted
tags); every blocking call carries a timeout. 5/5 clean runs, <1s each.

Impl: `src/kirito/stdlib_parallel.hpp` + `src/kirito/dispatcher.hpp`. Docs: `docs/pages/10-stdlib.md`
(`## parallel`) + `docs/pages/31-bonus-06-concurrency.md`.

## Verdict: no discrepancies. Docs match implementation.

Every documented surface was confirmed: `cpucount`, `spawn`→`Task`(`join`/`done`), `Queue`
(put/get/putnowait/getnowait/qsize/empty/full/close, bounded back-pressure, close-drain semantics),
`Lock`/`Event`/`Semaphore`/`Barrier` (acquire/release/set/clear/wait/parties/nwaiting/reset/abort,
timeouts, context-manager use), cross-VM transfer + identity, and unhashability.

## PINNED behaviours (exact, load-bearing — DO NOT let these regress silently)

Exact error strings (matched by `raises_msg` substrings; full text below):

- `spawn(42)` / `spawn(len, ...)` → `parallel.spawn: first argument must be a Kirito function
  (defined in a .ki file)` (non-function or native fn — thrown synchronously at spawn).
- `spawn()` → `parallel.spawn: missing function argument`.
- non-serializable **argument** → thrown at spawn: `cannot dump type 'Socket' (define
  _getstate_/_setstate_ to make it serializable)`. Same for `Queue.put(sock)`.
- worker throws → re-raised at `join` as `parallel: worker thrown: <message>` (e.g. `... : kaboom`).
- `Queue.get`/`getnowait` on empty → `parallel.Queue: get from an empty queue`.
- `Queue.putnowait` on full → `parallel.Queue: put to a full queue`.
- `Queue.get`/`put` on closed → `parallel.Queue.get: queue is closed` / `parallel.Queue.put: queue
  is closed` (get still drains buffered items first, then throws).
- `Queue(-1)` → `parallel.Queue: maxsize must be >= 0`; `Semaphore(-1)` → `parallel.Semaphore: value
  must be >= 0`; `Barrier(0)` → `parallel.Barrier: parties must be >= 1`.
- non-numeric `timeout` → `parallel: timeout must be a number or None`.
- `Lock.acquire` again by the same worker → `parallel.Lock.acquire: lock is not reentrant (already
  held by this worker)`; `Lock.release` of an unheld lock → `parallel.Lock: release of an unlocked
  lock`.
- `Barrier.wait` timeout (under-filled) → `parallel.Barrier.wait: barrier is broken` (breaks it).

Return-value / semantic pins:

- `Lock.acquire`/`Semaphore.acquire`/`Event.wait` return a **Bool** (True acquired/set, False on
  timeout — they do NOT throw on timeout). `Barrier.wait` is the exception: it **throws** on timeout.
- `join` is idempotent — a second call returns the cached value (or re-throws the cached error).
- Primitives compare `==` by **identity** (`q == q` True, `q == parallel.Queue()` False) and are
  **unhashable** (`{q}` → `unhashable type 'Queue'`), so they can't be Set/Dict keys.

## Notes (behaviour worth recording; not defects)

1. **A function LITERAL / lambda in the source file IS spawnable.** The docs stress "a function
   defined in a loadable `.ki` file (the worker re-reads that file and locates `fn` by source
   position)". Confirmed literally: `parallel.spawn(Function(x): return x*x, 5).join()` succeeds,
   because `findFunctionBySpan` (dispatcher.hpp) locates ANY `FunctionExpr` in the re-parsed program
   by line/col — it need not be a named top-level `var`. The only spawn-time rejections are a
   non-Function value, a native function, or a function whose `sourceFile` is empty/`<main>` (REPL /
   embedded). So the real constraint is "must live in a re-readable file", not "must be a named
   top-level function". The verify test therefore exercises the share-nothing failure with a genuine
   **closure** (a nested `inner` capturing an enclosing local), which fails at `join` with `name
   'captured' is not defined` — the closure does not cross, exactly as documented.

2. **The resolver rejects a module-level function that references a fully-unbound name at COMPILE
   time**, so you cannot even construct a "spawn a function using an undefined global" test — the
   share-nothing violation only manifests for a name bound in an enclosing *function* scope (a real
   closure). This is consistent with `resolver.hpp` behaviour, not a `parallel` issue.

3. **`Barrier.abort()`** makes later `wait()` throw (as documented — "permanent"), but the message is
   the generic `parallel: operation aborted (dispatcher shut down)` (WaitResult::Aborted), not a
   barrier-specific string. The verify test asserts only that it throws (via `raises`), leaving the
   exact wording unpinned since it is shared with dispatcher-shutdown aborts.

4. Fork-bomb guard (`parallel.spawn: cannot spawn from a module's top level ...`) is left to the
   existing `parallel_forkbomb_guard.ki` — it only fires during a worker's bootstrap re-load and is
   awkward to trigger deterministically from a single verify driver.
