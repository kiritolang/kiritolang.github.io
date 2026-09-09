# A6 — SERDE / SYS / CRYPTO deep audit (scan3, v1.17.1)

Scope: JSON, serialize/dump (serde core), hashing (MD5/SHA-1/SHA-256/384/512/HMAC/PBKDF2),
crypto (AES-GCM/RSA/ECDSA/X.509 via OpenSSL), zlib/deflate, base64, hex, sys, path.
Binaries: `build-bin/ki-asan` (TLS off) and `build-bin/ki-asan-tls` (OpenSSL, `crypto.enabled==True`).
Method: every claim reproduced with `.ki` user code; hash/crypto matched against published vectors;
serde + crypto entry points fuzzed under ASan/UBSan. No source edited, no build run.

## Result: ZERO confirmed bugs (HIGH/MED/LOW)

Nothing user-code-provable broke. All primitives byte-match published test vectors; all adversarial
inputs are rejected loudly with no silent-wrong-accept and no sanitizer hit. Two design observations
below are documented intended behavior (not counted as bugs per the hard rule).

---

## Informational observations (documented behavior, NOT counted)

### I1 — JSON integer > int64 silently widens to a lossy Float
`src/kirito/stdlib_json.hpp:206-212` · INFO (documented design) · CONFIRMED behavior
```
json.parse("9223372036854775808")            -> 9.22337203685478e+18   (2^63, was exact int text)
json.parse("123456789012345678901234567890") -> 1.23456789012346e+29
```
Values within int64 are exact (`9223372036854775807` round-trips; `9007199254740993` = 2^53+1 is
preserved). Past int64 the parser widens to `double` (and to ±Infinity past double range) instead of
throwing. This is a deliberate, in-code-documented choice ("widen to Float (mirroring dynamic
languages)"). It diverges from CPython's arbitrary-precision `json` (Kirito has no bigint-JSON path),
so a caller round-tripping huge integers through JSON loses precision *silently*. Flagging only as an
informational item — it is documented intent and reachable, but not a defect against the stated
contract. If the maintainer wants parity with the rest of Kirito (which HAS `int`/BigInt), widening a
>2^63 literal to a BigInt rather than a lossy Float would be the loss-free alternative; a decision call,
not a bug.

### I2 — JSON duplicate object keys: last value wins, silently
`src/kirito/stdlib_json.hpp:88-101` · INFO · CONFIRMED
```
json.parse('{"a":1,"a":2}')  -> {'a': 2}
```
Matches CPython `json` and JS `JSON.parse` (RFC 8259 leaves it unspecified). Not a defect; noted
because the brief called it out as a hunt target.

---

## Verified CORRECT (with the exact vectors matched)

### Hashing — every digest byte-matches its published KAT (`ki-asan`)
| primitive | input | output | source |
|-----------|-------|--------|--------|
| MD5 | "" | `d41d8cd98f00b204e9800998ecf8427e` | RFC 1321 |
| MD5 | "abc" | `900150983cd24fb0d6963f7d28e17f72` | RFC 1321 |
| SHA-1 | "abc" | `a9993e364706816aba3e25717850c26c9cd0d89d` | FIPS 180-4 |
| SHA-256 | "abc" | `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad` | FIPS 180-4 |
| SHA-256 | "" | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` | FIPS 180-4 |
| SHA-384 | "abc" | `cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed…` | FIPS 180-4 |
| SHA-512 | "abc" | `ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a…` | FIPS 180-4 |
| HMAC-SHA256 | key="Jefe", "what do ya want for nothing?" | `5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843` | RFC 4231 case 2 |
| HMAC-SHA256 | key="key", "The quick brown fox…lazy dog" | `f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8` | well-known |
| CRC-32 | "123456789" | `3421780262` = 0xCBF43926 | standard check |
| Adler-32 | "Wikipedia" | `300286872` = 0x11E60398 | standard |
| CRC-64/XZ | "123456789" | -7395533204333446662 = 0x995DC9BBDF1939FA | ECMA-182/xz check |

### PBKDF2 — matches RFC 6070 (SHA-1) and RFC 7914 (SHA-256) (`ki-asan`)
```
pbkdf2("password","salt",1,20,"sha1")    = 0c60c80f961f0e71f3a9b524af6012062fe037a6   (RFC 6070)
pbkdf2("password","salt",2,20,"sha1")    = ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957   (RFC 6070)
pbkdf2("password","salt",4096,20,"sha1") = 4b007901b765489abead49d926f721d065a429c1   (RFC 6070)
pbkdf2("passwd","salt",1,64,"sha256")    = 55ac046e56e3089fec1691c22544b605…d3a19783  (RFC 7914)
```
Defaults are sane: `iterations` is a **required** argument (no weak default); primitive `pbkdf2Raw`
hard-errors on `c<1` (stdlib_hash.hpp:284) and the binding rejects `iters<1` or `iters>2^32-1`
(stdlib_hash.hpp:78) so a >=2^32 value can't wrap to a tiny work factor. `dklen` bounded [1,1MiB].
`comparedigest` is constant-time on the equal-length path and returns False (no throw) on length
mismatch — verified True/False/False/True for equal/diff/diff-len/empty.

### OpenSSL / TLS-enabled crypto (`ki-asan-tls`, `crypto.enabled==True`)

**AES-GCM matches a published vector** (GCM spec / NIST test case 3, AES-128, 96-bit IV, no AAD):
```
key=feffe9928665731c6d6a8f9467308308  iv=cafebabefacedbaddecaf888
ct.hex() == 42831ec2…473f5985   ✓   tag.hex() == 4d5c2af327cd64a62cf35abd2ba6fab4   ✓
aesdecrypt(key, ct, iv, tag) == plaintext   ✓
```
Defaults / footguns: the ONLY symmetric mode exposed is AES-**GCM** (authenticated) — no ECB, no
unauthenticated CBC default. `gcmCipher` selects 128/192/256 by key length and throws on any other
size. IV is mandatory and empty-IV throws. RSA default is **2048** bits (`rsagenerate`, min 512 /
max 16384 enforced); RSA encryption is OAEP-SHA256 only; signatures are PKCS#1 v1.5 with a
caller-named digest.

**Safety / adversarial behavior — all correct, all reproduced via user `.ki`:**
- Tampered GCM tag → `aesdecrypt` throws "authentication failed" (never returns plaintext).
- Wrong AAD → throws. Truncated 15-byte tag → throws ("tag must be 16 bytes", stdlib_crypto.hpp:286).
- Wrong key length (20 B) → throws; empty nonce → throws.
- RSA sign/verify round-trip True; tampered message → False; OAEP encrypt/decrypt round-trips.
- ECDSA (prime256v1) sign/verify True; tampered message → False.
- Key/algorithm confusion: EC private key handed to `rsasign` → throws (requireKeyType,
  stdlib_crypto.hpp:138).
- TLS-off binary: every op throws "requires KIRITO_ENABLE_TLS" (gated, never silent no-op).

### serialize / dump code-execution gate — documented, CORRECT
`serialize.loads`/`dump.loads` re-parse and execute embedded Function/Class source. Confirmed a
serialized class's **eager class-variable initializers run at load time** (arbitrary code executes in
the loading VM). This is the pickle-analogy risk and it IS documented: `docs/pages/10-stdlib.md:324-327`
("Never `loads`/`load` a blob from an untrusted source … a blob is a program, not data") plus the
dedicated section `## Security: never load a blob you do not trust` (10-stdlib.md:1298-1303), which
explicitly states "`loads`/`load` runs code … eager class-variable initializers run right there, at
load time." Properly documented + steers users to `json` across trust boundaries. No fix needed.

### Codecs — round-trip + junk rejection (`ki-asan`)
- **base64** (kimodule, stdlib_kimodules.hpp:648): round-trips; rejects non-alphabet chars, data after
  padding, lone trailing char, non-canonical trailing bits ("aa==","ab=="); tolerates missing/extra
  padding and MIME whitespace (documented). No corruption path.
- **hex** (`fromhex`, bytes.hpp:249): rejects odd length and non-hex digits; skips whitespace
  (documented). No out-of-range coercion.
- **zlib/deflate**: compress/decompress and raw inflate/deflate round-trip; malformed streams throw
  cleanly (translated DeflateError), no silent partial output.

### sys / path — NUL handling & string ops (`ki-asan`)
- Poison-NUL truncation blocked: `sys.createprocess`/`shell` reject a NUL in argv/cwd
  (stdlib_sys.hpp:50-54); every filesystem-touching `path` entry rejects a NUL in the path
  (stdlib_path.hpp:87-91). Verified both throw.
- `path.splitext`/`join`/`dirname`/`basename` match os.path semantics on adversarial inputs
  (`.bashrc`→['.bashrc',''], `..`→['..',''], `archive.tar.gz`→['archive.tar','.gz'],
  join absolute-reset, dirname("/a")→"/", basename("/foo/bar/")→"").
- env access serialized under a process-wide mutex (stdlib_sys.hpp:36) against the getenv UAF race.

### Serde-blob fuzzing (ASan/UBSan, `ki-asan`) — ZERO hits
- 20,000 random-token blobs → all rejected, no crash.
- 3,000 mutation-fuzzed valid blobs (2 seeds; byte-flip / truncate / digit-insert on List/Dict/Set/
  Function/class blobs) → 104 still parsed as valid, 2,896 rejected cleanly, **0 ASan/UBSan hits**.
  The reader's whole-token integer parsing (requireWhole), length/count bounds (countToken bounded by
  blob size), and rebuild's per-link `checkId` bounds checks hold under mutation.

### Crypto fuzzing (ASan/UBSan, `ki-asan-tls`) — ZERO hits, ZERO false-accepts
- `aesdecrypt` × 8,000 malformed (random tag/ct/key/nonce, varied lengths) → **0 accepted**, all threw.
- `ecverify` × 4,000 random signatures → **0 false-accepts**.
- `rsaverify` × 3,000 mutated PEMs → 1,032 rejected as malformed PEM, remainder returned False,
  **0 false-accepts**.
- `x509parse` × 2,000 garbage certs → all threw. No sanitizer hit in any run.

## Missing-test note (coverage)
Existing `tests/unit/test_crypto.cpp` covers the TLS crypto KATs well. No `.ki` golden/error script
pins the JSON >2^63 widening (I1) or duplicate-key last-wins (I2) as intended behavior; if the
maintainer keeps them, a golden test naming the behavior would prevent silent drift. base64 non-canonical
rejection has an `.experr` only for the encode side; a decode-side `.experr` for "aa==" / lone-trailing
would lock in the corruption-rejection guarantee.
