# A12 Net/Sys/Time

Status: DONE

Scope: src/kirito/stdlib_net.hpp, src/kirito/net_compat.hpp, src/kirito/stdlib_sys.hpp,
src/kirito/proc_compat.hpp, src/kirito/stdlib_time.hpp.

Reviewed README.md false-positives table first (sys.exit skips fstreams — noted, will not re-flag).

Read in full: stdlib_net.hpp (1600 lines), net_compat.hpp, stdlib_sys.hpp, proc_compat.hpp,
stdlib_time.hpp. Cross-checked against test_net.cpp, test_net_primitives.cpp, test_net_tls.cpp,
test_proc.cpp, test_sys.cpp, test_sys_binary.cpp, test_sys_time_deep.cpp, test_time.cpp, and the
`.ki` golden scripts (verify_net/labx_net/r6-r10_net/spec_net*/probe_http_client_abuse/audit_net,
sys_proc*, verify_time/labx_sys_time/r6_time/audit_time/deep_system).

Overall impression: this subsystem has been through several prior audit rounds (v1.14, v1.14.1,
1.12.1) and is unusually hardened already — overflow-checked DateTime arithmetic, a portable
civil-calendar epoch<->fields conversion independent of platform gmtime range limits, a
grandchild-killing process-group timeout for createprocess/shell, an exec-error pipe for clean
"program not found" errors, drain threads with capture caps to avoid OOM/deadlock, CRLF-injection
guards on HTTP headers/cookies/multipart fields, and an explicit credential-stripping redirect
policy (NET-1). Findings below are the residual gaps found after this baseline.

### A12-1: `net.Session` cookie jar and default headers are not host-scoped — cross-origin leak  [MEDIUM] [medium]
- **Location**: src/kirito/stdlib_net.hpp:1019-1045 (`sessionDo`), :1022-1035 (`mergeInto`)
- **What**: `sessionDo` unconditionally merges the *entire* `Session.cookiesH` jar and `Session.headersH`
  into every outgoing request's `opts`, regardless of which host the call targets — there is no
  per-cookie Domain/Path tracking at all (Set-Cookie is folded into the jar as a flat name->value
  Dict, dropping any `Domain=`/`Path=` attribute). Compare this to the *single-request* redirect
  path (`netRequest`'s `net::redirectScope`, NET-1), which explicitly strips `Authorization` and
  clears the cookie jar when a redirect crosses to a different hostname — i.e. the codebase clearly
  cares about this exact class of leak for redirects, but a `Session` object making two independent
  calls to different hosts has no equivalent guard. A real `requests.Session` scopes cookies by
  domain (via `http.cookiejar`); this one does not.
- **Repro** (conceptual, no external network needed — provable against two local listeners):
  ```
  var s = net.Session()
  s.get("http://host-a/login")          # host-a sets Set-Cookie: session=SECRET
  s.headers["Authorization"] = "Bearer token-for-host-a"
  s.get("http://host-b/track")          # host-b receives Cookie: session=SECRET
                                         # AND Authorization: Bearer token-for-host-a
  ```
  Both the cookie and the header travel to `host-b`, which the user did not authorize for either.
- **Proposed fix**: store each cookie's originating host (and optionally path) alongside its
  value in the jar (e.g. `{name: {value, domain, path}}` instead of a flat `name->value` Dict), and
  in `sessionDo` filter by suffix-domain-match against the target URL's host before merging —
  reusing `net::redirectScope`'s host-comparison logic. Headers are harder to scope generically
  (real `requests.Session` also sends default headers to every host), so at minimum this should be
  documented as a known limitation if left as-is; the cookie case is the more clearly unintended gap
  given the redirect-path precedent.
- **Proposed test**: two local `Socket` listeners acting as `host-a`/`host-b` (loopback, different
  ports — Kirito's HTTP client doesn't validate hostname vs the connected IP, so `127.0.0.1:PORT_A`
  and `127.0.0.1:PORT_B` suffice to stand in for different origins); assert that a cookie set by
  `host-a`'s response is *not* present in the `Cookie:` header of a subsequent `session.get` to
  `host-b`.

### A12-2: `net.socketpair()` mislabels its POSIX AF_UNIX sockets as `family == "inet"`, and getsockname/getpeername silently return `["", 0]` on them  [LOW] [medium]
- **Location**: src/kirito/stdlib_net.hpp:1457-1471 (`socketpair` ctor call sites,
  `SocketVal(fds[0], AF_INET, typ)` / `fds[1], AF_INET, typ`); src/kirito/net_compat.hpp:216-218
  (`socketPair` — native `::socketpair(AF_UNIX, type, 0, out)` on POSIX); src/kirito/stdlib_net.hpp:185-195
  (`net::formatAddr`, silently clears host/port on a `getnameinfo` failure instead of throwing)
- **What**: On POSIX, `net.socketpair()` creates a genuine `AF_UNIX` socket pair (confirmed in
  `net_compat.hpp`), but the Kirito wrapper hardcodes `AF_INET` for both ends regardless — so
  `pair[0].family` reports `"inet"` for a socket that is not, in fact, an Internet-family socket
  (on Windows it genuinely is AF_INET, since that platform emulates the pair over a loopback
  listener — so the lie is POSIX-only, a portability inconsistency). Downstream, calling
  `getsockname()`/`getpeername()` on one of these sockets performs the real `::getsockname`/
  `::getpeername` syscall (which correctly returns an `AF_UNIX` sockaddr), then `net::formatAddr`
  calls `::getnameinfo` on it — `getnameinfo` only understands `AF_INET`/`AF_INET6` and fails
  (`EAI_FAMILY`) for `AF_UNIX` — and on that failure `formatAddr` swallows the error, setting
  `host=""`/`port=0` rather than throwing. So `net.socketpair()[0].getsockname()` returns
  `["", 0]` instead of either the Unix-domain path (empty for an unnamed pair, arguably correct)
  or, more usefully, a clear "not supported for a Unix-domain socket" error — the current behaviour
  looks like a successful call with real (if empty-ish) data, not a caught limitation.
- **Repro**:
  ```
  var pr = net.socketpair()
  print(pr[0].family)        # "inet" — but this is really an AF_UNIX socket on Linux
  print(pr[0].getsockname()) # ["", 0] — silently "succeeds" with garbage rather than throwing
  ```
- **Proposed fix**: either (a) track the *actual* underlying family (introduce an `"unix"` family
  string on POSIX) so `.family` is truthful and `getsockname`/`getpeername` can special-case it, or
  (b) have `formatAddr` throw when `getnameinfo` fails instead of silently zeroing the output, so
  at least the failure is visible rather than indistinguishable from success.
- **Proposed test**: `assert net.socketpair()[0].family == "inet"` currently passes but is
  arguably testing the bug; add a test that either pins the corrected `"unix"` label or asserts
  `getsockname()` on a socketpair end throws / is documented, whichever fix is chosen.

### A12-3: `socket.settimeout(seconds)` accepts a negative value and silently no-ops instead of throwing  [LOW] [high]
- **Location**: src/kirito/stdlib_net.hpp:1188-1198 (`settimeout` binding); src/kirito/stdlib_net.hpp:286-299
  (`net::setTimeout`, `if (seconds <= 0) return;`)
- **What**: `settimeout(-1)` (or any negative value) is accepted without error: `net::setTimeout`
  treats "not positive" as "leave blocking / no timeout" and simply returns, and the socket's
  `s.timeout` member is still stored as the negative value (used later to bound `connect()` via
  `connectWithTimeout`, whose own `if (seconds <= 0) return ::connect(...)` guard likewise treats it
  as "no bound"). Python's `socket.settimeout(-1)` raises `ValueError: Timeout value out of range`;
  Kirito's silently normalizes to "blocking", which could mask a caller bug (e.g. a computed
  timeout that went negative due to an upstream error) rather than surfacing it.
- **Repro**: `net.tcpsocket().settimeout(-1)` returns `None` with no error, and a subsequent
  `connect()` on that socket is unbounded (blocking) rather than raising — no test in the suite
  exercises `settimeout` with a negative argument.
- **Proposed fix**: reject `seconds < 0` explicitly in the `settimeout` binding (`0` remains the
  documented "blocking / no timeout" sentinel, matching existing semantics; only the negative case
  is a genuine input-validation gap).
- **Proposed test**: `assert raises(Function(): return net.tcpsocket().settimeout(-1))`.

### A12-4: HTTP client does not special-case a `100 Continue` interim response  [LOW] [low] — COVERAGE GAP / weak spot
- **Location**: src/kirito/stdlib_net.hpp:545-582 (`net::parseRaw`)
- **What**: `parseRaw` treats the first `\r\n\r\n`-delimited chunk of the raw response as the one
  and only status line + header block. A server that legitimately sends an HTTP/1.1 `100 Continue`
  interim response before the real final response (normally gated behind the client sending
  `Expect: 100-continue`, which Kirito's client never does — so in practice this is very unlikely
  to trigger against a compliant server) would have its `100 Continue\r\n...\r\n\r\n` block parsed
  as if it were the final response, and the real status line/headers that follow would be
  misinterpreted as body content. Low likelihood given the client never sends `Expect:
  100-continue`, but a misbehaving/aggressive server could still emit one unprompted.
- **Repro**: COVERAGE GAP — no test feeds a raw `HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\n...`
  response through `parseRaw`/`net.get` against a local test listener.
- **Proposed fix**: after parsing a status line, if `status == 100`, skip that header block and
  re-parse from the next `\r\n\r\n` onward (loop until a non-1xx final status).
- **Proposed test**: a local `Socket` listener that replies with a `100 Continue` block followed
  by the real `200 OK` response; assert the client surfaces the final status/body, not the interim
  one.

## COVERAGE GAPS (enumerated, no bug demonstrated)

- `net.socketpair()[0/1].getsockname()`/`getpeername()` — untested (see A12-2).
- `net.Socket().settimeout(-1)` / other negative timeouts — untested (see A12-3).
- HTTP `100 Continue` interim response — untested (see A12-4).
- `sys.createprocess`/`sys.shell` with a *negative* `timeout` — falls through the same
  `timeoutSecs > 0` guard as `0`/`None` (silently "no timeout"); not distinguished from the
  documented "positive timeout kills" contract in a test. Likely intentional (mirrors
  `settimeout`'s `<= 0` = "no timeout" convention) but unconfirmed by a pinned test either way.
- `time.DateTime` as a Dict/Set key with many distinct instants — hashing (`std::hash<int64_t>`)
  is exercised only incidentally; no dedicated `Set`/`Dict`-of-DateTime stress test.
- `net.getaddrinfo`/`gethostbyname`/`gethostname` — exercised only for the success path against
  `localhost`/loopback; no test pins an unresolvable hostname's error message shape.
- IPv6 bind/connect/sendto/recvfrom on `::1` is unit-tested, but an IPv6 HTTP client dial
  (`http://[::1]:port/`) is not exercised by the golden `.ki` suite.
- `net`'s TLS surface (`Socket.starttls`, `cipher()`) error-path messages under a non-TLS build ARE
  exercised (confirmed in test_net.cpp) — noted only for completeness, not a gap.

## Summary

Subsystem (net/sys/time) is unusually well-hardened from prior audit rounds: overflow-checked
DateTime arithmetic, a portable civil-calendar epoch<->fields conversion, grandchild-killing
process-group timeouts for createprocess/shell, an exec-error pipe for clean spawn-failure errors,
capped/threaded drain loops, CRLF-injection guards on HTTP headers/cookies/multipart, and an
explicit credential-stripping redirect policy (NET-1). No memory-safety or crash-class bugs found.

4 findings, all LOW/MEDIUM:
- **A12-1 (MEDIUM)**: `net.Session`'s cookie jar and default headers are not host-scoped — a
  Session making calls to two different origins leaks cookies (and headers) between them, unlike
  the single-request redirect path which explicitly guards against exactly this (NET-1 precedent).
  The most substantive finding in this scan.
- **A12-2 (LOW)**: `net.socketpair()` mislabels its POSIX AF_UNIX pair as `family == "inet"`, and
  `getsockname`/`getpeername` on such a socket silently return `["", 0]` instead of throwing when
  `getnameinfo` fails on the AF_UNIX sockaddr.
- **A12-3 (LOW)**: `socket.settimeout(-1)` silently no-ops instead of raising (Python raises
  ValueError for a negative timeout).
- **A12-4 (LOW, coverage gap)**: HTTP client doesn't special-case a `100 Continue` interim
  response; low real-world likelihood since the client never sends `Expect: 100-continue`.

Plus several enumerated coverage gaps (none demonstrated as bugs): socketpair address
introspection, negative-timeout paths (both net and sys), 100-Continue, DateTime-as-key stress,
unresolvable-hostname error message, and IPv6 HTTP client dial.

Status: DONE.
