# `net` module — doc-vs-impl verification (loopback / no external network)

Test: `tools/tests/scripts/verify_net.ki` (+ `.expected` = `OK net\n`), 118 asserts (113 top-level +
5 in IPv6/dgram conditional blocks). Runs in ~24 ms, all on 127.0.0.1 / socketpair — NO external
network.
Impl: `src/kirito/stdlib_net.hpp` (+ `net_compat.hpp`). Docs: `docs/pages/10-stdlib.md` `## net`.
Runner: `build-debug/ki`. **No `src/` edits.**

Scope: the HTTP client (`request`/`get`/…/`Session`/`Response`) and TLS are intentionally NOT covered
here (they need a live server / the public internet); see `spec_net*.ki` for the server-driven suites.
CRLF header/cookie injection guards are HTTP-client-only and left to those.

## Discrepancies

**None** that are doc-vs-impl mismatches. One behavioural sharp edge is pinned below.

## Coverage

- **Build flag**: `net.tlsenabled` is a `Bool`.
- **URL helpers (exact round-trips)**:
  - `quote` — unreserved set (`A-Za-z0-9-_.~`) untouched; space -> `%20`, literal `+` -> `%2B`,
    reserved `/?=&#` encoded, UTF-8 multi-byte (`café` -> `caf%C3%A9`), empty -> empty.
  - `unquote` — `%20`/`+` -> space, `%2B` -> `+`, UTF-8 decode, passthrough, **malformed `%zz`
    passes through literally**; `quote`/`unquote` round-trip a space+plus payload.
  - `urlencode` — `k=v&...` with keys+values encoded; empty Dict -> `""`.
  - `parseqs` — decodes; **duplicate keys last-wins**, **empty-key pair dropped**, **bare key ->
    `""` value**, empty query -> `{}`; `urlencode`/`parseqs` round-trip.
  - `urlsplit` — scheme/host/port/path/query/fragment all `String`; **port is textual digits**
    (empty when absent); **userinfo `user:pass@` stripped** from host/port; **IPv6 brackets kept**
    (`[::1]`) with the `:port` after `]`.
- **Socket constructors + attributes**: `Socket()` (inet/stream), `socket(family, type)` (+ keyword
  args), `tcpsocket()`, `udpsocket()`; read-only `.family` (`inet`) / `.type` (`stream`/`dgram`);
  `.family` assignment throws (read-only). IPv6 (`inet6`) asserted **only when the host supports it**
  (probed — see notes).
- **socketpair**: `[Socket, Socket]`, default `stream`, send returns byte count, bytes both
  directions; datagram pair (POSIX) exercised behind a capability probe.
- **TCP loopback echo (single process)**: `bind("127.0.0.1", 0)` ephemeral + `getsockname` port,
  `listen`, `connect`, `accept` (inherits family/type), `getpeername`/`getsockname` agree,
  `send`/`recv` (recv -> **Bytes**), echo back, `shutdown("write")` surfaces as EOF (empty recv),
  `recvall` drains until peer half-closes.
- **UDP loopback**: `sendto` returns byte count, `recvfrom` -> `[Bytes, [host, port]]`, binary-exact
  datagram (NUL/0xFF/CRLF), `connect` sets default peer so plain `send`/`recv` work.
- **Options**: `setsockopt`/`getsockopt` for `reuseaddr`/`rcvbuf`/`sndbuf`; named conveniences
  `setnodelay`/`setkeepalive`/`setreuseaddr`/`setbroadcast`; getsockopt-only `type` (1=STREAM,
  2=DGRAM), `error` (0 fresh), `acceptconn` (0 before listen); `setblocking`, `settimeout`.
- **Lifecycle**: `fileno()` (>=0 open, **-1 after close/detach**), `detach()` returns the fd and
  clears it, `fromfd()` adopts it (type/family as given, live fd). `with net.Socket() as s:` closes
  on block exit.
- **Name resolution**: `gethostname()` non-empty; `gethostbyname("127.0.0.1")`==identity,
  `gethostbyname("localhost")`==`127.0.0.1`; `getaddrinfo("127.0.0.1", 80)` -> List of
  `{family, type, host, port}` dicts, with the `family`/`type` filter narrowing results.

## Hardening (exact messages PINNED)

- bad family    -> `unknown address family '<x>' (expected 'inet' or 'inet6')`
- bad type      -> `unknown socket type '<x>' (expected 'stream' or 'dgram')`  (also `socketpair`)
- unknown option (set/get) -> `setsockopt: unknown option '<x>' (valid: reuseaddr, broadcast,
  keepalive, rcvbuf, sndbuf, nodelay, reuseport)` / `getsockopt: unknown option ...`
- port out of 0-65535 (connect/bind/sendto) -> `port out of range: <n> (must be 0-65535)`
- negative recv/recvfrom size -> `recv size must be non-negative` / `recvfrom size must be non-negative`
- bad shutdown how -> `shutdown: how must be 'read', 'write', or 'both' (got '<x>')`
- any op on a closed/detached socket -> `<op>: socket is closed` (recv/send/getsockname/setsockopt);
  **second `detach()` -> `detach: socket is closed`** (never hands the same fd out twice).
- `getpeername` on an unconnected socket throws; `send(<non-str/bytes>)` throws; missing required
  method args (`connect(host)` with no port, `send()`) throw a clean arity error, not UB.

## Pinned current behaviour / notes (not doc bugs)

- **`recv(0)` BLOCKS** — it does NOT return an empty `Bytes` immediately. The impl calls
  `::recv(fd, buf, 0, 0)` (`net_compat.hpp` `recvBytes`), which on Linux blocks awaiting data rather
  than short-circuiting on a zero length (Python's `socket.recv(0)` special-cases an immediate
  `b''`). This is a latent hang footgun but is undocumented and out of the "returns up to n bytes"
  contract, so it is NOT asserted here (asserting it would hang the suite). Recorded so a future
  reader knows it is intentional/observed, not a test gap. Candidate for a `src/` fix (short-circuit
  `n == 0`) if it is ever deemed a defect — flagged, not touched.
- **IPv6 (`inet6`) is not creatable in every environment** (this sandbox returns
  `socket() failed: Address family not supported by protocol`). The docs correctly list `inet6`; the
  test asserts the IPv6 path only behind a runtime capability probe, so it neither fails nor is
  skipped silently where IPv6 works.
- `gethostbyname`/`getaddrinfo` for `localhost`/`127.0.0.1` resolve numerically with no live DNS, so
  they are safe in an offline sandbox.
- `detach()` then `fromfd()` round-trips a fd within one process; `fileno()` on the detached original
  correctly reports `-1` (the old number must not leak out to be re-adopted).
