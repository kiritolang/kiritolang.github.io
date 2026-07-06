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
