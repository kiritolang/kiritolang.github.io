# Coverage findings — `net` and `parallel` stdlib modules

Test: `tests/scripts/cov_net_parallel.ki` (+ `.expected`). 136 `assert`s plus
`threw`/`emsg` hardening checks. Verified against `build-bin/ki-release`; output is
byte-identical across 8 runs. Deterministic, no external network, no external deps.

## Bugs / silent errors

None. Every should-error case errored cleanly with a specific, catchable diagnostic;
no silent failures or silent fallbacks observed.

## Behavioral notes / doc-deltas (not bugs)

1. **Kirito functions are serializable, so passing a function as a `spawn` argument does
   NOT throw.** The docs' phrase "a non-serializable argument is caught synchronously at
   spawn" is correct, but a *function* is serializable by design (source text travels), so
   `par.spawn(fn, otherFn)` succeeds. The genuinely non-serializable case is a
   **live-resource native** (e.g. a `Socket`), which DOES throw synchronously at spawn:
   `cannot dump type 'Socket' (define _getstate_/_setstate_ to make it serializable)`.
   Repro:
   ```
   var takef = Function(f): return 1
   par.spawn(takef, someOtherFunction)   # no throw — a function serializes
   par.spawn(takef, net.Socket())        # throws at spawn — a socket does not
   ```

2. **A non-serializable RETURN value surfaces at `join` wrapped as a worker throw.** Docs
   say it "surfaces later, at join"; concretely `join` re-throws
   `parallel: worker thrown: cannot dump type 'Socket' (...)`. Consistent with docs, just
   noting the exact wrapping (it comes through the same "worker thrown" channel as any
   in-worker exception).

3. **`net.urlsplit` does not parse a network-path (protocol-relative) reference.**
   `net.urlsplit("//host/path")` yields `scheme=""`, `host=""`, `path="//host/path"` —
   the authority is only recognized after a literal `"://"`. Reasonable (urlsplit is a
   pure splitter, and `resolveUrl` handles `//host` separately for redirects), but the
   `urlsplit` doc entry does not mention this case. The test deliberately does NOT rely on
   network-path parsing.

## net functions NOT testable without a network (out of scope for a golden test)

Only their existence + clean error-on-bad-input is covered here; live success paths need
the network / a live server / DNS and are intentionally excluded:

- HTTP client: `request`, `get`, `post`, `put`, `delete`, `patch`, `head`, `options`,
  and `Session` — tested only that they reject a non-http(s) URL cleanly
  (`URL must start with http:// or https://`). No live request is made.
- `Response` object (`.status`/`.text`/`.json()`/`.raiseforstatus()`/indexing) — requires
  a real response, not exercised.
- Remote socket I/O: `connect`/`send`/`recv`/`recvall`/`accept`/`listen` to a real peer,
  and UDP `sendto`/`recvfrom` — covered only via the LOCAL `socketpair` (loopback,
  in-process) and via error paths (connect to a closed local port throws; closed-socket
  ops throw). No listening socket is opened and no external host is contacted.
- `starttls` / `cipher` / `is_tls` (TLS upgrade) — needs a TLS peer.
- `gethostbyname` / `getaddrinfo` — tested only on **numeric IP literals** (`127.0.0.1`),
  which resolve with no DNS lookup; hostname resolution needs the network.

## What IS fully covered in-process (deterministic, no network)

- net URL helpers: `quote`/`unquote` (incl. `+`, malformed-escape passthrough, round-trip),
  `urlencode`/`parseqs` (dup-key last-wins, empty-key drop, `+`→space, round-trip),
  `urlsplit` (port, IPv6 brackets, userinfo stripping, minimal).
- net sockets: constructor family/type reporting (`socket`/`tcpsocket`/`udpsocket`/`Socket`),
  LOCAL `socketpair` bidirectional send/recv, `with`-context auto-close, `setsockopt`/
  `getsockopt`, `fileno()==-1` on closed, `tlsenabled` type, and all input-validation errors.
- parallel: `cpucount`, `spawn`/`join` correctness (map == sequential, order preserved),
  idempotent `join`, worker-throw propagation, non-serializable arg/return errors,
  `Queue` FIFO + bounded + close + cross-worker producer + worker-created-queue-by-identity,
  `Lock` (non-reentrant, ctx-mgr, unheld-release), `Event` (cross-VM set), `Semaphore`
  (bounded permits, ctx-mgr), `Barrier` (cross-worker rendezvous, abort), constructor
  validation, and primitive unhashability.
