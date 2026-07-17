# A13 IO/Path/Hash/Crypto/Compression/Random/JSON

Status: complete.

Scope covered: `src/kirito/stdlib_io.hpp`, `stdlib_path.hpp`, `stdlib_hash.hpp`, `hashing.hpp`,
`stdlib_crypto.hpp`, `stdlib_zlib.hpp`, `stdlib_gzip.hpp`, `stdlib_random.hpp`, `rand_compat.hpp`,
`stdlib_json.hpp`. Read the false-positives table in `.audit/README.md` first (io.write stringifies
Bytes, BytesIO.seek clamps at 0 vs File.seek throws, sys.exit skips fstreams) — none re-flagged here.

Method: full read of all ten files, cross-checked against `tools/tests/unit/test_{io,path,hash,
hash_crypto,crypto,json,zlib,secure_random,random,io_path_deep,compress_hash_deep,random_net_deep}.cpp`
and the `.ki` golden scripts (`r4_io`, `r6_hash/io/json/random`, `r7/r8/r9_io_*`, `spec_*`, `verify_*`,
`labx_*`), then adversarially exercised the built `build-release/ki` binary directly (this round's base
is already extremely hardened — v1.12/v1.12.1/v1.13/v1.14 covered nearly everything obvious).

Confirmed correct by direct execution (standard test vectors / adversarial probes, all passed):
- `hash.md5/sha1/sha256/sha384/sha512("")` match the standard empty-input digests exactly.
- `hash.hmac` matches a known HMAC-SHA256 test vector; `hash.pbkdf2` matches RFC 6070 vectors #1
  (1 iteration) and #5 (4096 iterations, 25-byte multi-block output, PBKDF2-HMAC-SHA1).
- `hash.crc64("123456789")` matches the CRC-64/XZ check value `0x995DC9BBDF1939FA` (reinterpreted
  signed, as documented).
- String-vs-Bytes equivalence holds for `hash.sha256`/`hash.md5` on identical UTF-8 content.
- `json.loads` correctly: rejects `nesting too deep` past 1000 levels of `[`, rejects trailing
  data (`"1 2"`), rejects whitespace-only input, decodes a UTF-16 surrogate pair (😀, U+1F600)
  correctly, substitutes U+FFFD for an unpaired high/low surrogate (`\uD800`, `\uDC00` alone),
  round-trips control characters (`\tx`) through `stringify`→`loads`, and duplicate keys
  resolve to last-value-wins (`{"a":1,"a":2}` → `{'a': 2}`), the conventional JSON behavior.
- `path.join()` (zero args) throws; `path.join` performs no traversal normalization (by design,
  pure string op — `path.join("/a","../../etc/passwd")` → `/a/../../etc/passwd` verbatim).
- `path.chmod` on a missing path returns `False` (lenient, as documented); `path.getsize` on a
  missing path throws.
- `gzip.decompress` on 10 garbage bytes throws `gzip: stream too short`; on a truncated real gzip
  stream (5 bytes chopped off the end) throws `gzip: truncated stream`; a full round-trip via
  `Bytes.encode()/.decode()` works.
- `zlib.decompress` on an arbitrary non-zlib string throws a clean `zlib: unsupported zlib
  compression method` rather than crashing.
- `random.randombelow(0)` and negative throw; `hash.pbkdf2(..., 0)` throws `iterations must be >= 1`
  (already guarded in `stdlib_hash.hpp:74`, not a live bug despite looking suspicious in
  `hashing::pbkdf2Raw`'s raw loop, which would otherwise under-iterate silently for `iterations==0`
  since `for (i=1;i<iterations;...)` never executes — the wrapper's `iters < 1` guard is the only
  thing preventing that from being reachable; worth defensively hardening `pbkdf2Raw` itself, see
  A13-2 below, since it is a shared primitive).
- `io.BytesIO` write is capped at 256 MiB (`BytesIO too large` on `seek(3e8); write(...)`).
- Duck-typed streams work for `stream=` write (custom `write(self, data)` method); a duck stream
  missing `readline` throws a clean `'Duck2' object has no attribute 'readline'` from `io.input`.
- `io.open` with an invalid mode string (`"x"`) throws `unsupported file mode 'x'`; opening a file
  under a nonexistent directory throws `could not open file`.
- AES-GCM: correct key-length validation (rejects an 8-byte key), tag-length validation (rejects
  a 16-byte-but-wrong tag with a clean "authentication failed"), and a full encrypt/decrypt
  round-trip against a NIST GCM known-answer vector (verified via the existing `test_crypto.cpp`,
  re-confirmed live).

## Findings

### A13-1: `crypto.rsasign`/`rsaverify` and `ecsign`/`ecverify` perform no key-type check — silently cross-dispatch to whatever algorithm the PEM key actually is  [MEDIUM] [HIGH]
- **Location**: `src/kirito/stdlib_crypto.hpp:125-149` (`pkeySign`/`pkeyVerify`, shared by both
  `rsasign`/`rsaverify` and `ecsign`/`ecverify`), wired at lines 313-341 (RSA) and 389-417 (EC).
- **What**: `pkeySign`/`pkeyVerify` call the generic OpenSSL `EVP_DigestSign`/`EVP_DigestVerify`
  API, which dispatches purely on the *actual* key type loaded from the PEM — not on which Kirito
  function name was called. So `crypto.rsasign(ecPrivateKeyPem, msg)` silently produces a valid
  **ECDSA** signature (not RSA PKCS#1 v1.5, despite the function name and the module's documented
  contract "RSA (rsagenerate..., rsasign/rsaverify [PKCS#1 v1.5])"), and `crypto.ecverify` will
  happily verify an RSA signature produced by `crypto.ecsign` on an RSA key, and vice versa via
  `rsaverify`. Only `rsaencrypt`/`rsadecrypt` are protected (they call RSA-specific padding `ctrl`s
  that hard-fail on a non-RSA `EVP_PKEY_CTX`, confirmed live — `rsaencrypt` on an EC public key
  throws `operation not supported for this keytype`). The sign/verify path has no equivalent
  guard, so a caller who accidentally hands `rsasign` an EC key (a plausible mistake when key
  material is threaded through generic PEM-string plumbing) gets no error at all — just a
  different, silently-substituted signature algorithm. This breaks interop expectations (e.g. a
  downstream system expecting genuine RSA-PKCS1v1.5 bytes to feed into an RSA-only verifier would
  get bytes that don't even parse as an RSA signature) and is a textbook key/algorithm-confusion
  footgun.
- **Repro**:
  ```
  var crypto = import("crypto")
  var kp = crypto.ecgenerate()
  var sig = crypto.rsasign(kp["private"], "hello")   # no error — but this is an ECDSA sig, not RSA!
  io.print(len(sig))                                  # ~70 bytes (DER ECDSA), not a ~2048-bit RSA sig
  io.print(crypto.ecverify(kp["public"], "hello", sig))   # True — cross-function verify "works"
  io.print(crypto.rsaverify(kp["public"], "hello", sig))  # ALSO True — rsaverify accepts an EC key/sig

  var rk = crypto.rsagenerate(1024)
  var sig2 = crypto.ecsign(rk["private"], "hello")   # no error — actually an RSA PKCS#1 signature
  io.print(len(sig2))                                 # 128 bytes (RSA-1024), not ECDSA-shaped
  io.print(crypto.rsaverify(rk["public"], "hello", sig2))  # True
  ```
  Confirmed live against the TLS-enabled `build-release/ki`.
- **Proposed fix**: in `pkeySign`/`pkeyVerify` (or at the four call sites), check
  `EVP_PKEY_base_id(key.get())` against the expected type (`EVP_PKEY_RSA` for `rsasign`/
  `rsaverify`, `EVP_PKEY_EC` for `ecsign`/`ecverify`) and throw a clear `"rsasign: expected an RSA
  private key, got a <type> key"` (mirroring the existing `aes: key must be 16/24/32 bytes` style)
  before calling `EVP_DigestSignInit`/`VerifyInit`.
- **Proposed test**: a `test_crypto.cpp` case (or `.ki` script) that generates an EC keypair and
  asserts `CHECK_THROWS(evalStr(vm, "c.rsasign(e[\"private\"], \"m\")"))` and the symmetric
  `c.ecsign(r["private"], "m")` on an RSA key; likewise for `rsaverify`/`ecverify` given a
  signature+key of the wrong family.

### A13-2: `hashing::pbkdf2Raw` silently under-iterates (or could produce U1-only output) if ever called with `iterations == 0` — currently unreachable from Kirito, but a landmine for the next caller of the shared primitive  [LOW] [MEDIUM]
- **Location**: `src/kirito/hashing.hpp:283-306` (`pbkdf2Raw`), specifically the inner loop
  `for (uint32_t i = 1; i < iterations; ++i)`.
- **What**: `pbkdf2Raw` has no internal validation that `iterations >= 1`. If `iterations == 0`,
  the loop condition `1 < 0` (both `uint32_t`) is false, so the loop body never executes and the
  function silently returns `U1` (the single-iteration HMAC) as if `iterations == 1` had been
  requested — a *wrong, non-throwing* result rather than a clear error. Today this is masked
  because the only call site (`stdlib_hash.hpp:74`, `pbkdf2` Kirito builtin) validates
  `iters < 1` and throws before ever reaching `pbkdf2Raw`. But `pbkdf2Raw` is a shared
  `kirito::hashing` primitive documented as reusable by any C++ embedder/module author (per the
  "HashAlgo descriptor table" DRY design), and it gives no defense of its own — a future direct
  caller (a new stdlib module, or an embedding host bypassing the `pbkdf2` Kirito builtin) would
  silently get a 1-iteration derivation while believing it asked for 0 (or would get UB-adjacent
  behavior for a caller who reads `iterations` as "attacker-controlled" and forgot the `>=1`
  contract is enforced two layers up).
- **Repro**: not reachable via Kirito source today (guarded at the stdlib boundary); a direct
  C++ unit test calling `kirito::hashing::pbkdf2Raw(algo, "pw", "salt", /*iterations=*/0, 20)`
  demonstrates the silent 1-iteration fallback.
- **Proposed fix**: add `if (iterations < 1) throw std::invalid_argument("pbkdf2: iterations must be >= 1");`
  (or an assert) at the top of `pbkdf2Raw` itself, so the invariant lives with the primitive, not
  only with today's one caller.
- **Proposed test**: a small C++ unit test (or extend `test_hash.cpp`) directly calling
  `hashing::pbkdf2Raw` with `iterations=0` and asserting it throws/asserts rather than returning a
  1-iteration digest.

## Coverage gaps (not bugs, just untested combinations worth adding)

- `crypto.rsasign`/`ecsign`/`rsaverify`/`ecverify` × wrong-key-family (see A13-1) — no test exists
  today (`test_crypto.cpp` only exercises same-family sign/verify + PEM-garbage rejection at
  line 134).
- `hash.comparedigest` — tested for equal-length equal/unequal, but no test asserts the
  length-mismatch path explicitly returns `False` rather than throwing (the code does return
  `False`, confirmed by reading, but not pinned by a test per the file's own comment about
  "leaks length too").
- `random.Random(seed=X)._getstate_()`/`_setstate_()` round-trip across a *process restart*
  (serialize to a String, write to a file, read back in a fresh VM, continue the stream) is only
  exercised in-process in the existing suite; an explicit two-VM (or dump-to-disk-and-reload)
  round-trip isn't present, though the underlying serde plumbing is shared with other native types
  and independently tested elsewhere.
- `path.rename`/`path.chmod` cross-filesystem or read-only-parent-directory failure modes (only
  missing-path and success paths are covered).
- `io.open(..., "r+b")` (binary read/write) round-trip: explicit mode combination not found in a
  dedicated test, though `"r+"` (text) and `"...b"` (binary read/write separately) both are.
- `gzip`/`zlib` decompression-bomb ceiling (`kMaxInflateOut`) is exercised for the aggregate-budget
  case (multi-member gzip) per the A16-1 comment, but no test targets the zlib-module single-shot
  `decompress` against a maximal expansion ratio input (this lives in `deflate.hpp`, outside my
  file assignment, so flagging only as a heads-up for whichever agent owns that file).

## Summary

This subsystem is extremely well-hardened by four prior audit rounds — every adversarial probe I
tried (deep JSON nesting, malformed gzip/zlib, zero/negative counts, path-join edge cases, PBKDF2
iteration-count validation, BytesIO overflow, duck-typed stream gaps, permission/missing-file
opens) was already correctly guarded and threw clean, catchable errors. The one live, confirmed
bug is **A13-1**: `crypto.rsasign`/`rsaverify`/`ecsign`/`ecverify` don't validate that the supplied
PEM key matches the named algorithm family, so passing an EC key to an "rsa" function (or vice
versa) silently succeeds with a *different* signature scheme instead of throwing — a real
key/algorithm-confusion footgun, MEDIUM severity (no confidentiality break, but a clear
correctness/interop contract violation with high reproduction confidence). **A13-2** is a
defense-in-depth LOW finding in the shared `pbkdf2Raw` primitive (currently unreachable from
Kirito, since the one call site already guards `iterations >= 1`, but the primitive itself has no
guard and is documented as reusable). No HIGH/memory-safety issues found in this subsystem this
round.
