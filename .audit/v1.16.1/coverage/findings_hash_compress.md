# Coverage findings — hash / crypto / gzip / zlib (v1.16.1)

Binary: `./build-bin/ki-release` (ki 1.16.1, `crypto.enabled == True` — this build has KIRITO_ENABLE_TLS).
Test: `tests/scripts/cov_hash_compress.ki` (118 `assert`s + `threw`/`emsg` checks), exits 0.

## Result: no bugs, no silent errors, no doc deltas.

Every published test vector reproduced exactly and every should-error case errored with the
documented message substring. Specifics verified against independent references:

- **SHA/MD5**: empty + "abc" FIPS/canonical vectors for md5/sha1/sha256/sha384/sha512; the 1e6×'a'
  SHA-256 vector; the 0..255 byte SHA-256 vector. String and equal-bytes Bytes hash identically.
- **HMAC**: HMAC-SHA256("key","The quick brown fox…") == f7bc83f4… (canonical). All five algo names
  accepted; empty key/msg valid; long key folded; unknown algo throws the listed-set message.
- **PBKDF2**: RFC 6070 SHA-1 vectors for c=1/2/4096 (dklen 20) all match. Bounds enforced:
  iterations ∈ [1, 4294967295], dklen ∈ [1, 1048576] (max dklen 1048576 accepted at the boundary).
- **checksums**: adler32("")==1, adler32("abc")==0x024D0127; crc32("")==0, crc32("abc")==0x352441C2;
  crc64("123456789")==0x995dc9bbdf1939fa (CRC-64/XZ check value; signed −7395533204333446662).
- **gzip/zlib**: round-trip of empty/text/binary-0..255/100 KB inputs; type preservation
  (String→String, Bytes→Bytes); gzip magic 1f 8b 08; multi-member concat decodes+joins; raw
  deflate/inflate; a zlib stream is correctly rejected by `inflate` (header confusion caught).
- **hardening (all throw, no silent partial output)**: gzip too-short / bad-magic / truncated-trailer
  / corrupt-body-CRC / trailing-junk; zlib too-short / garbage / corrupt-Adler; inflate garbage.
- **crypto (TLS build)**: AES-256-GCM NIST vector (zero key/IV, empty PT → tag 530f8afb…);
  encrypt/decrypt round-trip incl. AAD; tampered tag / wrong AAD throw "authentication failed"
  (never returns unauthenticated plaintext); bad key length rejected; RSA sign/verify (tampered
  msg → False) + OAEP encrypt/decrypt; EC sign/verify; wrong key family throws
  "expected RSA private key"; x509parse rejects a non-cert PEM.

## Notes (expectation vs. surface — NOT bugs)

- **No incremental hasher API.** The task brief mentions `update()/hexdigest/digest`; neither the
  docs (10-stdlib.md §hash) nor `stdlib_hash.hpp` expose an incremental hasher — the module is
  one-shot only (`md5(data)→String`, etc.). Nothing to test; docs make no such claim, so no delta.
- **No compression `level` parameter.** The task brief mentions "levels"; `gzip.compress` /
  `zlib.compress` (and `deflate`) take only `data` — matches the docs exactly. Extra positional
  args would be an arity error; the feature simply does not exist.
- **crypto is compiled in here** (`crypto.enabled == True`), so the "requires KIRITO_ENABLE_TLS"
  gate branch could not be exercised on this binary. The test branches on `crypto.enabled` and
  asserts the gate messages when it is False, so it stays correct on a non-TLS build too.
