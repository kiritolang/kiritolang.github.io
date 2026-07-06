# net subsystem audit — v1.15

Source: `src/kirito/stdlib_net.hpp`, `src/kirito/net_compat.hpp`
Probe: `./build-debug/ki`. CI has NO external network — loopback (127.0.0.1) only.

## LOG
- Read stdlib_net.hpp (1470 lines) + net_compat.hpp (232 lines) fully.
- Probed URL helpers (quote/unquote/parseqs/urlsplit/urlencode): all behave; multibyte/astral quote correct (per-UTF8-byte), %GG/%4 malformed pass through literally, percentDecode bounds correct.
- Probed socket construction/validation/lifecycle: family/type strings validated, port range checked BEFORE resolution, use-after-close throws on every op, fileno=-1 after close/detach, double-detach throws, setsockopt/getsockopt string keys + read-only enforcement all correct, shutdown validation correct, recv(-1)/recvfrom neg rejected.
- Probed loopback: socketpair byte-exact (NUL/0xFF), recv(0)=empty immediately, UDP sendto/recvfrom byte-exact, datagram-too-large throws, TCP bind/listen/connect/accept byte-exact. All good.
- Probed HTTP response parsing on loopback (spawned canned server): chunked, gzip, deflate all decode; malformed chunk size -> empty body (no crash); truncated chunk -> appends remainder+framing (safe, no over-read); Set-Cookie folded; case-insensitive header() works.
- Probed redirect security: cross-origin (port change) strips Authorization (correct), keeps cookies for same-hostname different-port (CORRECT per RFC 6265 host-scoping + documented policy); hostname change (127.0.0.1->localhost) drops cookies (correct). Custom headers preserved across redirect.
- RULED OUT (by-design/correct): quote encodes '/' (Kirito has no safe= param; divergence from Python but not a bug); percentDecode bounds; redirect method 303/POST->GET; recvAll 256MiB cap; SIGPIPE ignore.

## FINDINGS

### F1 [MED] net.unquote turns '+' into space (should be literal '+')
- where: src/kirito/stdlib_net.hpp:321-342 (percentDecode) used by unquote at :1435
- repro: `net.unquote("a+b")` => "a b"
- actual: "+" decoded to space. expected: Python's urllib.parse.unquote leaves "+" untouched ("a+b"); only unquote_plus / query-string parsing converts "+". percentDecode is shared between parseqs (where +->space is correct) and unquote (where it is wrong).
- fix idea: give percentDecode a `plusToSpace` flag (true for parseqs, false for unquote), or a separate decode-without-plus path for unquote. Note DRY: one function serves two different contracts.

### F2 [MED] HTTP client parseUrl rejects userinfo (user:pass@host) URLs with a misleading error
- where: src/kirito/stdlib_net.hpp:249-299 (parseUrl); contrast splitUrl at :346-377 which DOES strip userinfo
- repro:
  - `net.get("http://user:pass@127.0.0.1:1/p")` => THROW "invalid port in URL ...: 'pass@127.0.0.1:1'"
  - `net.get("http://user@127.0.0.1:1/p")`      => THROW "could not resolve host 'user@127.0.0.1'"
- actual: the `@userinfo` is not stripped, so it lands in the host/port and either the port parse or DNS fails with a confusing message. expected: a legitimate URL form (requests supports it, and Kirito's own urlsplit handles it) should either work (strip userinfo, optionally use it for Basic auth) or give a clear error. Inconsistent with splitUrl/urlsplit which correctly does `hostport.rfind('@')`.
- fail-safe note: NOT an SSRF vector — `google.com@evil.com` is passed whole to getaddrinfo and fails to resolve rather than connecting to evil.com. So it's a usability/correctness/consistency bug, not a security hole.
- fix idea: strip `user:pass@` in parseUrl the same way splitUrl does (rfind('@')); optionally auto-add an Authorization header from it.

### F3 [LOW] r.headers Dict collapses duplicate response headers to the last value
- where: src/kirito/stdlib_net.hpp:721-733 (makeResponse builds a Dict from the ordered header vector via hd.set, so repeated keys overwrite)
- repro: response with `X-H: one` then `X-H: two` => `r.header("x-h")` == "two"; only one X-H key in r.headers.
- actual: duplicate headers collapse (last wins). expected: requests exposes a case-insensitive multi-value view; a lossless header list would be ideal. NOTE: this is cosmetic only for the cookie path — multiple Set-Cookie ARE all folded into the jar because the internal fold loop iterates the ORDERED HttpResult.headers vector (verified: 3 Set-Cookie => all 3 cookies present). So the important case is safe; only the user-facing r.headers Dict is lossy.
- fix idea: for repeated headers, join with ", " (HTTP field-combining rule) or expose a list; low priority.

## RULED OUT (verified correct, not bugs)
- URL/header/cookie CRLF+control-char injection: all rejected (parseUrl scans for <0x20/0x7F; setHdr and cookie loop reject CR/LF). Redirect Location re-parsed through parseUrl so a server-controlled CRLF Location is also rejected.
- Port validation happens BEFORE name resolution in connect/bind/sendto (checkPort) and in parseUrl (numeric-only + 1..65535).
- Cross-origin redirect credential handling matches documented policy + RFC 6265: Authorization stripped on hostname/port/scheme change (except std http->https upgrade); cookies dropped only on hostname change (host-scoped, not port-scoped). Verified end-to-end.
- Socket lifecycle: use-after-close throws on every op; fileno=-1 after close/detach; double-detach throws; detach relinquishes ownership.
- setsockopt/getsockopt read-only enforcement (error/type/acceptconn get-only); bogus option rejected with valid-list.
- Loopback TCP/UDP/socketpair byte-exact incl NUL/0xFF; recv(0)=empty immediately (no hang); sendto datagram-too-large throws; recv(-1) rejected.
- chunked decode (incl `;ext`), gzip/deflate decode, invalid gzip swallowed (body left as-is), malformed chunk size -> empty body (no crash), truncated chunk -> appends remainder without over-read (dechunk bounds guard).
- Response.content is byte-exact w.r.t. wire bytes; json()/raiseforstatus()/ok/header() correct.
- recvAll 256 MiB cap (net::kMaxRecvAll) shared by plain-TCP and TLS SSL_read loops; sendBytes/recvBytes clamp to 16 MiB to avoid int overflow of the Winsock length arg.
- gunzip flag/index handling bounded by the initial size>=18 check and `while (i<size)`/`i+8>size` guards.

## LOG (continued)
- Probed injection (CRLF/NUL/tab/DEL in URL, CRLF in headers/cookies, bad ports/schemes): all correctly rejected.
- Probed malformed responses, dup headers, Set-Cookie folding (multi + '=' in value), getaddrinfo numeric, binary content byte-exactness, userinfo URLs.
- DONE. 3 findings (F1 MED unquote+, F2 MED userinfo, F3 LOW dup-header collapse). Core socket + HTTP + injection surface is robust.
NET F1 = FALSE POSITIVE (by-design, documented): net.unquote decodes + to space because it follows urllib.parse.unquote_plus, per docs/pages/10-stdlib.md:631. NOT a bug.
