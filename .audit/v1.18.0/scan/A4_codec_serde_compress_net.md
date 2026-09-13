# A4 — Codecs: serialize/serde, compression, hashing/crypto, base64/hex, json, net

Probed on build-asan/ki + build-debug/ki; reference blobs + cross-check via python3. One MED.

## Finding (MED, CONFIRMED) — net response decode skips the integrity trailer
`stdlib_net.hpp:510-546`: `net::gunzip` strips the 8-byte gzip trailer without checking CRC-32/ISIZE,
and the `deflate` branch calls raw `inflate` ignoring the zlib Adler-32 — so a corrupt-but-decodable
HTTP body is silently accepted. The standalone `gzip.decompress`/`zlib.decompress` validate correctly,
so `net::gunzip` is also a weaker SSOT-dup. Repro: a gzip stream with a flipped body byte (still
inflates) → `gzip.decompress` rejects (CRC mismatch) but `net.request` returned the corrupted body.
→ FIXED (#1: route through gzipfmt::decompress / deflate::zlibDecompress).

## VERIFIED CORRECT
- serde (KSER1 text+binary): round-trips (empty/nested/cycles/shared-ref identity/-0.0/π/INT64_MAX/
  embedded whitespace/sets); crafted-blob rejection (bad header/tag/out-of-range id/negative count/
  trailing data/junk-glued-token/truncated/odd-pairs); 2400 mutated blobs under ASan → 0 crashes.
- compression: zlib/raw-deflate/gzip round-trips (all-256-byte ×4, empty, 22 KB); bidirectional python
  interop; rejections (zip-bomb size cap, corrupt CRC/Adler, truncated, bad method, stored NLEN); 2100
  mutated blobs → 0 crashes.
- hashing/crypto: md5/sha1/sha256/384/512/hmac/adler32/crc32 + pbkdf2 vs python; pbkdf2 weak-KDF
  guards (iterations 0 and ≥2^32 rejected — no uint32 wrap); AES-GCM round-trip + tamper/short-tag/
  AAD-binding auth failures; RSA/EC sign+verify, key-type confusion blocked, RSA-OAEP.
- base64 (all bytes, RFC vectors, invalid/post-pad/lone-char rejection, element-range validated) + hex
  (odd-length + non-hex rejection).
- json: depth guard (1000), cycle detection, shortest-round-trip floats, surrogate handling, control
  escaping, indent overflow guard.
- net: parseUrl rejects bad scheme / CR-LF injection / bad port; header/cookie/multipart CRLF-injection
  rejection; redirect credential stripping; DEFAULT 10 s timeout verified firing (~1.01 s vs blackhole).

## Informational (LOW, no security impact) — #11
`net.urlsplit` (lenient informational splitter) accepts malformed IPv6 `http://[::1/path` (host='[')
where Python urlsplit raises. Never feeds a real request (those use the strict parseUrl). Noted only.
