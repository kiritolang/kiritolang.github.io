# A18 — net + net_compat audit (v1.13)

Scope: `src/kirito/stdlib_net.hpp` (1445 lines), `src/kirito/net_compat.hpp` (173 lines).
Focus: the recently-EXPANDED socket foundation (UDP/IPv6/socketpair/fromfd/options/name-resolution)
layered on the existing TCP + HTTP/1.1 client + TLS.

Method: static read-only review + test-coverage cross-check against
`tools/tests/unit/{test_net,test_net_primitives,test_net_tls,test_random_net_deep,test_audit_v112,test_audit_findings}.cpp`
and `tools/tests/scripts/{spec_net,spec_net_primitives,spec_net_tls,r4/r6/r7/r8/r9/r10_net*,probe_http_client_abuse}.ki`.

Legend: severity {info,low,medium,high}; confidence {low,med,high}.
Confirmed/likely bugs first, then coverage gaps.

---
## Confirmed / likely bugs

### A18-1: CRLF header injection via user-supplied `headers`, `cookies`, and folded Set-Cookie names
- severity: medium
- location: stdlib_net.hpp:824-828 (`setHdr`), :838-840 (headers dict), :843-846 (cookies option), :870-881 (request builder + Cookie header), :891-898 (Set-Cookie fold)
- category: security / injection
- description: The request line/host and redirect `Location` are guarded against control chars
  (parseUrl rejects `<0x20`/`0x7F`, NET-1), but user-supplied header VALUES and header KEYS are
  written verbatim into the request: `req += k + ": " + v + "\r\n"`. Nothing strips/rejects `\r`/`\n`
  in `headers={...}` values/keys, in `cookies={...}` values, or in the `auth`-adjacent path. A value
  like `"a\r\nX-Injected: 1"` splits into two headers; a value ending `"\r\n\r\nGET /evil..."` enables
  request smuggling. Python's http.client/requests reject `\r`/`\n` in header values for exactly this.
- failure-scenario: `net.get(url, {"headers": {"X-Token": userInput}})` where userInput (a
  username, an API token echoed from elsewhere, a form field) contains CRLF → attacker controls
  arbitrary extra headers / a smuggled second request to the same origin.
- proposed-test: assert `net.get(url,{"headers":{"X":"a\r\nEvil: 1"}})` either throws or the wire
  bytes captured by a loopback server contain no injected `Evil:` header; same for a cookie value.
- proposed-fix: in `setHdr` (and the cookie-jar builder) reject any key/value containing `\r` or `\n`
  with a clear `KiritoError("header value contains a control character")`; percent-encoding is not
  appropriate for header values, so rejection is correct.
- confidence: high

### A18-2: TLS `SSL_new` / `SSL_set_fd` return values unchecked; SSL_CTX leak on `SSL_new` failure
- severity: low
- location: stdlib_net.hpp:599-601
- category: robustness / resource leak
- description: `SSL* ssl = SSL_new(ctx); SSL_set_fd(ssl, (int)fd); SSL_set_tlsext_host_name(ssl, ...)`.
  If `SSL_new` returns nullptr (OOM / bad ctx), `SSL_set_fd(nullptr, …)` is UB, and `ctx` + `fd` leak
  (the error paths that free ctx are only reached after a successful SSL_connect branch). `SSL_set_fd`
  and the SNI macro return codes are also ignored.
- failure-scenario: allocation failure or an exotic OpenSSL build → crash or leak instead of a clean
  `KiritoError`.
- proposed-test: hard to unit-test (needs OpenSSL fault injection); document as defensive.
- proposed-fix: `if (!ssl) { SSL_CTX_free(ctx); netcompat::closeSocket(fd); throw KiritoError("SSL_new failed"); }`
  and check `SSL_set_fd`.
- confidence: high (unchecked), low (that it triggers in practice)

### A18-3: `detach()` leaves `fd` intact (only sets `closed`); detach-twice / fileno-after-close return a stale fd
- severity: low
- location: stdlib_net.hpp:1201-1218 (fileno, close, detach), :1216
- category: use-after-close / double-close hazard
- description: `detach()` returns `s.fd` and sets `s.closed = true` but does NOT reset `s.fd` to
  `kInvalidSocket`. Consequences: (a) calling `detach()` twice returns the SAME fd twice → two
  `net.fromfd` adopters both own it → cross-VM double-close/EBADF later; (b) `fileno()` (:1202-1204)
  reads `s.fd` with no `closed` check, so after `close()`/`detach()` it returns a now-closed/recycled
  fd — Python's `fileno()` returns -1 on a closed socket. A caller who `fromfd`s that recycled number
  operates on an unrelated fd.
- failure-scenario: `var f = s.fileno(); s.close(); net.fromfd(f)` adopts whatever now owns that fd
  number; or `s.detach(); s.detach()` hands the same fd to two workers.
- proposed-test: `var s=net.tcpsocket(); discard s.detach(); CHECK_THROWS(s.detach())` (should throw
  "socket is closed"); `s.close(); s.fileno()` should throw or return -1.
- proposed-fix: in `detach` set `s.fd = kInvalidSocket` after capturing it; guard `fileno` with
  `fdOrThrow("fileno")` (or return -1 when closed).
- confidence: high

### A18-4: `settimeout`/`timeout` does not bound `connect()` — a black-hole host hangs regardless
- severity: medium
- location: stdlib_net.hpp:227-240 (setTimeout sets SO_RCVTIMEO/SO_SNDTIMEO only), :1012-1024 (Socket.connect uses a plain blocking `::connect`), :578-579/652-653 (httpExchange → dialTcp → blocking connect, then setTimeout AFTER connect)
- category: hang / resource
- description: `setTimeout` only sets receive/send timeouts; on POSIX these do NOT bound a blocking
  `connect()`. For the HTTP client, `setTimeout(fd, timeout)` is applied only AFTER `dialTcp` has
  already returned (i.e. after connect completed), so `net.get(url, {"timeout": T})` to a firewalled
  IP that silently drops SYNs blocks for the OS default connect timeout (tens of seconds to minutes),
  not `T`. `Socket.connect(host, port)` has no timeout path at all.
- failure-scenario: `net.get("http://10.255.255.1/", {"timeout": 1})` hangs far longer than 1s.
- proposed-test: point at a non-routable/unaccepting address with a small timeout and assert the call
  returns within a bounded multiple of the timeout. (Environment-sensitive — mark carefully.)
- proposed-fix: for the connect path, use non-blocking connect + `select`/`poll` with the timeout, or
  document that `timeout` covers only send/recv (matching Python's requests connect-vs-read split).
- confidence: high (behavior), med (severity in practice)

### A18-5: No-timeout HTTP read relies solely on `Connection: close`; a keep-alive/hostile peer hangs until OOM ceiling or forever
- severity: low (partly by design)
- location: stdlib_net.hpp:213-224 (recvAll), :490 (parseRaw ignores Content-Length), :882 (client always sends `Connection: close`)
- category: hang / weak-spot
- description: The body is delimited purely by the peer closing the connection (`recvAll` loops until
  `recv`==0). Content-Length is never used to stop reading. A server that returns a Content-Length but
  keeps the socket open (ignores `Connection: close`), or one that slow-drips, will hang the client
  when no `timeout` is set — bounded only by the 256 MiB `kMaxRecvAll` ceiling if it keeps sending, or
  indefinitely if it stalls. The `probe_http_client_abuse.ki` script only exercises this WITH a
  timeout.
- failure-scenario: `net.get(hostileUrl)` (no timeout) against a stalling server → indefinite hang.
- proposed-test: loopback server that sends full headers + Content-Length then sleeps without closing;
  assert a default (no-timeout) request eventually terminates, or document the requirement to pass a
  timeout.
- proposed-fix: honor Content-Length to stop reading once the declared body is in; and/or apply a
  sane default timeout.
- confidence: med

### A18-6: `dechunk` chunk size parsed into `long` (32-bit on LLP64/Windows) and via `strtol` accepts `0x`/sign
- severity: low
- location: stdlib_net.hpp:435 (`long sz = std::strtol(sizeHex.c_str(), nullptr, 16);`)
- category: portability / parser robustness
- description: `long` is 32-bit on Windows (LLP64), so a legitimate >4 GiB chunk header (or a crafted
  `ffffffff...`) truncates/overflows; `strtol` on overflow returns `LONG_MAX` (then treated as a huge
  size → `out.append(body, i, npos)` dumps the rest — benign but sloppy). `strtol` also silently
  accepts a leading `0x` and a `-` sign in the chunk-size field, neither of which is valid chunk
  grammar. Bodies are already capped by recvAll's 256 MiB ceiling upstream, limiting impact.
- failure-scenario: cross-platform inconsistency in how a malformed/oversized chunk header is handled.
- proposed-test: feed a chunk header `"100000000\r\n..."` and one with junk (`"-5"`, `"0x5"`) and
  assert consistent, non-crashing behavior.
- proposed-fix: parse with `std::size_t`/`unsigned long long` via `std::strtoull` and validate the
  hex digits explicitly.
- confidence: med

### A18-7: `fromfd(fd)` adopts any integer with no validation; int64→`int` truncation on POSIX
- severity: low
- location: stdlib_net.hpp:1319-1326, :87-88 (adopting ctor)
- category: validation / cast
- description: `fromfd(fd, family, type)` casts an arbitrary `int64` to `netcompat::socket_t`
  (`int` on POSIX) with no range/validity check and `closed=false`. `fromfd(2_000_000_000_000)`
  truncates; `fromfd(-1)` yields an fd that then reports "socket is closed" via fdOrThrow (OK), but
  `fromfd(999999)` yields a plausible-but-dead fd whose operations surface raw errno strings rather
  than a domain error. Intended for same-process detach handoff, but nothing enforces provenance.
- failure-scenario: a wrong fd number silently binds the Socket to an unrelated/closed OS fd.
- proposed-test: `net.fromfd(-1).recv(4)` throws; `net.fromfd(999999).getsockname()` throws cleanly
  (already likely, but assert it).
- proposed-fix: reject fd outside `[0, INT_MAX]`; optionally `fcntl(fd, F_GETFD)`/getsockopt(SO_TYPE)
  probe and throw if the fd isn't a valid socket.
- confidence: med

### A18-8: `resolveUrl` mishandles a scheme-relative (`//host/path`) redirect Location
- severity: low
- location: stdlib_net.hpp:379-389
- category: URL parsing edge case
- description: `resolveUrl` treats any `loc[0]=='/'` as root-relative and prefixes `origin`, so a
  protocol-relative `Location: //other.example/x` becomes `origin + "//other.example/x"` (e.g.
  `http://host.example//other.example/x`) — a wrong path on the ORIGINAL host, not a redirect to
  `other.example`. RFC 3986 protocol-relative references are legal (browsers/requests honor them).
- failure-scenario: a server issuing a protocol-relative redirect is followed to the wrong URL
  (functionality bug; also a subtle security surprise since the origin doesn't actually change so
  auth/cookies are NOT stripped even though the intended host differs).
- proposed-test: 302 with `Location: //127.0.0.1:PORT2/x`; assert the client resolves to that host or
  at least does not silently target the original host's `//...` path.
- proposed-fix: detect a leading `//` and treat the remainder as `scheme:` + authority.
- confidence: med

### A18-9: `listen(backlog)` / accept path — negative/absent backlog and unbounded recv buffers not validated
- severity: info/low
- location: stdlib_net.hpp:1042-1048 (listen: `static_cast<int>(asInt(...))`, no range check), :1066/:1107 (recv/recvfrom cap at 64 MiB per call)
- category: validation
- description: `listen(-1)` passes a negative backlog straight to `::listen` (OS-clamped, harmless but
  unvalidated). `recv(n)`/`recvfrom(n)` cap the allocation at 64 MiB per call, but a script calling
  `recv(64*1024*1024)` in a loop allocates 64 MB each time — bounded per call, so only a mild DoS
  vector on the caller's own VM. Noting for completeness; not a crash.
- proposed-fix: none required; optionally clamp/validate backlog.
- confidence: high

---

## Coverage gaps (tested? — gap logged)

### A18-10: socket options `rcvbuf`/`sndbuf`/`acceptconn`/`reuseport` are untested
- severity: low
- location: stdlib_net.hpp:166-179 (lookup tables), tests: none (grep for rcvbuf/sndbuf/acceptconn/reuseport in tools/tests → 0 hits)
- category: coverage
- description: Only `reuseaddr`, `nodelay`, `broadcast`, `keepalive`, `type`, `error` are exercised
  (test_net_primitives.cpp). The buffer-size options (`rcvbuf`/`sndbuf` — get AND set round-trip),
  the getsockopt-only `acceptconn` (should read 1 on a listening socket, 0 otherwise), and the
  conditionally-compiled `reuseport` have no test.
- proposed-test: set `sndbuf`/`rcvbuf`, read back (kernels often double the value — assert `>0` or
  `>= requested`); `listen()` then assert `getsockopt("acceptconn") != 0`.
- confidence: high

### A18-11: `socketpair("dgram")` (POSIX AF_UNIX datagram) untested; only default stream pair tested
- severity: low
- location: stdlib_net.hpp:1302-1316, net_compat.hpp:157-159; tests: test_net_primitives.cpp:63-72 and spec_net_primitives.ki:41 use only the default (stream)
- category: coverage
- description: `net.socketpair("dgram")` is a supported form on POSIX (`::socketpair(AF_UNIX,
  SOCK_DGRAM, ...)`) and returns `false` (→ throw) on Windows. Neither the datagram pair nor the
  Windows-emulation dgram-rejection is tested.
- proposed-test: POSIX: `net.socketpair("dgram")` round-trips a datagram both ways. (Windows path is
  untestable on Linux CI — document.)
- confidence: high

### A18-12: multipart `files` with the BARE-content form `{field: content}` (no [filename, content] list) untested
- severity: low
- location: stdlib_net.hpp:796-808 (the `else` branch at :802-803 handles a non-List value)
- category: coverage
- description: Both unit tests (test_net.cpp:377, :393) use the two-element `[filename, content]` list
  form. The alternate `{field: content}` form — where filename defaults to the field name (:797) — is
  never exercised, nor is a bare-Bytes content value in that form.
- proposed-test: `post(url, {"files": {"f": "raw"}})` and `{"files": {"f": Bytes([1,2,3])}}`; assert
  `filename="f"` and the raw bytes appear in the captured multipart body.
- confidence: high

### A18-13: end-to-end cross-host redirect that DROPS Authorization/cookies over the wire is untested
- severity: low
- location: policy at stdlib_net.hpp:417-423 + application at :903-921; test_audit_v112.cpp:164-181 unit-tests `redirectScope` PURELY (no live server)
- category: coverage
- description: The pure policy function `redirectScope` is well unit-tested, but there is no
  loopback-server test that a real 302 to a DIFFERENT host/port actually omits the `Authorization`
  header and clears the cookie jar on the second (redirected) request's bytes. Also the same-host
  http→https standard-port upgrade KEEPING auth is only unit-tested at the policy level.
- proposed-test: two loopback servers (or two paths distinguished by Host); GET with `auth=` +
  `cookies=`; server1 redirects to server2; assert server2's captured request has no Authorization and
  no Cookie header.
- confidence: high

### A18-14: `getaddrinfo` filter args (family/type/service-name/IPv6 results) and `gethostbyname` of a real name are thinly tested
- severity: low
- location: stdlib_net.hpp:1343-1373 (getaddrinfo), :1333-1341 (gethostbyname)
- category: coverage
- description: test_net_primitives.cpp:139-144 only calls `getaddrinfo("127.0.0.1", 80)` (no family/
  type filter, numeric only) and `gethostbyname` on a loopback literal. Untested: `getaddrinfo(host,
  port, "inet6", "dgram")` filtering, a String service name (`"http"` — the code passes portStr to
  getaddrinfo which accepts services), an IPv6 result's `host` formatting, `getaddrinfo("")`
  (host.empty → nullptr node), and the empty-family/empty-type default (`AF_UNSPEC`/`0`) shape. A
  real-name resolution is deliberately avoided (non-deterministic) — acceptable.
- proposed-test: `getaddrinfo("::1", 80, "inet6")` shape; `getaddrinfo("localhost")` returns ≥1 with
  `family` in {inet,inet6}.
- confidence: med

### A18-15: TLS request-side robustness (large POST body over SSL_write, chunked/partial SSL_write loop) untested
- severity: low
- location: stdlib_net.hpp:627-632 (SSL_write send loop)
- category: coverage
- description: test_net_tls.cpp covers large/chunked/gzip RESPONSE bodies and verify/hostname paths
  thoroughly, but every request is a small GET. The `SSL_write` partial-write loop (a large `data=`/
  `json=`/`files=` POST body over TLS) is never exercised. Also `net.get("https://...")` in a NON-TLS
  build is asserted to throw only in test_net.cpp (guarded by `#ifndef KIRITO_ENABLE_TLS`) — fine.
- proposed-test: POST a ~1 MiB body over the in-process TLS server; assert the server received the
  full body and the response round-trips.
- confidence: med

### A18-16: `parseUrl`/`splitUrl` adversarial URL corpus is thin
- severity: low
- location: stdlib_net.hpp:248-298 (parseUrl), :345-376 (splitUrl)
- category: coverage / weak-spot
- description: Only a handful of URL edge cases are asserted (control-char rejection is guarded in
  code but not directly tested; test_audit_findings covers bracketed IPv6 in urlsplit). Untested:
  `http://` with empty host, `http://host:99999/` (out-of-range port → should throw),
  `http://[::1/` (unterminated bracket → should throw), `http://[::1]junk` (junk after ']' → should
  throw), a URL containing a raw `\r`/`\n` (should throw "control character"), userinfo stripping in
  splitUrl (`http://user:pass@h/`→host `h`). The parseUrl port validation (:287-296) and IPv6-bracket
  branches (:274-286) have code but no direct negative tests.
- proposed-test: a table of malformed URLs each asserted to throw a clear KiritoError; a userinfo URL
  asserted to strip the credentials from `host`.
- confidence: high

---

## Notes / non-findings (checked, OK)
- GC-safety of list/dict building in `recvfrom` (:1115-1123), `getsockname/getpeername` (:1149-1152),
  `getaddrinfo` (:1361-1372), and `makeResponse` (:717-729): all use `RootScope`/pinned wrapper
  ctors and root each fresh handle before the next allocation — looks correct.
- `gunzip` header-flag skipping (:446-462) is bounded (size≥18 precheck, per-flag `i<s.size()` loops,
  final `i+8>s.size()` throw) — no obvious OOB.
- `dechunk` terminates (i strictly advances or breaks) and guards `i+sz>body.size()` before append.
- `closeFd` is idempotent (`isValid(fd) && !closed`); destructor closes — no double-close from one
  object.
- `sendto` datagram size guarded (:1091) before the syscall; `sendTo`/`recvBytes` clamp to 16 MiB and
  cast length to `int` only after clamp (no overflow).
- Redirect body/method handling (303 + POST→GET downgrade clears body + Content-Type; 307/308 keeps
  both) is correct; Content-Length recomputed each hop, stale Content-Length header skipped (:872).
- percentDecode/percentEncode bounds and the `%XX`-at-EOF case are handled.
- httpExchange fd/SSL/CTX cleanup on the throwing and success paths is balanced (except the SSL_new
  null case, A18-2).
