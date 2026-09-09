# A6 — Serialization / System / Compression / Crypto+Hash audit (v1.17.1)

Scope: `stdlib_json.hpp`, `stdlib_serde.hpp`, `stdlib_serialize.hpp`, `stdlib_dump.hpp`,
`stdlib_io.hpp`, `stdlib_path.hpp`, `stdlib_sys.hpp`, `stdlib_time.hpp`, `deflate.hpp`,
`stdlib_zlib.hpp`, `stdlib_gzip.hpp`, `stdlib_crypto.hpp`, `stdlib_hash.hpp`, `hashing.hpp`,
`stdlib_random.hpp`, plus `base64` (`stdlib_kimodules.hpp`).

Method: no recompile. Reproduced on `build-bin/ki-release` and `build-bin/ki-asan`
(`ASAN_OPTIONS=detect_leaks=0`, `ulimit -s 262144`). Hashes checked against published vectors;
crypto checked against Python `cryptography`/OpenSSL; codecs fuzzed under ASAN with mutated blobs.

## Verdict

**No memory-safety defects, no OOB, no silent codec coercion bugs, no weak crypto defaults found.**
Every hunted class (round-trip fidelity, malformed-input handling, checksum correctness, KDF work
factor, RNG determinism, decompression bombs, seek/read bounds, NUL/path handling) came back clean or
matched a documented, deliberate design choice. The one high-impact security property (code execution
when deserializing untrusted `serialize`/`dump` blobs) is real, reproduced, and **already documented
with an explicit pickle-analogy warning**. Findings below are all INFO/LOW and documented.

---

## Findings

### F1 [INFO / CONFIRMED] `serialize.loads` / `dump.loads` execute embedded code on load (pickle-class)
`stdlib_serde.hpp:568-571` (`vm.evalIn(nd.s, ...)` re-runs class source in pass 0c; `:424` re-parses
function source). A serialized `class` carries its verbatim source; on load into a VM where the class
is absent, `rebuild` re-parses and **runs** the class body / class-variable initializers.

Reproducer (two separate processes / fresh VMs):
```
# producer
class Pwned:
    var x = sys.shell("echo INJECTED_CODE_RAN > /tmp/pwn_evidence.txt")
ser.save(Pwned,"/tmp/evil.kser")
# consumer (does NOT define Pwned)
var obj = ser.load("/tmp/evil.kser")   # -> /tmp/pwn_evidence.txt now contains INJECTED_CODE_RAN
```
Confirmed: the shell command ran during `ser.load`. Same for `dump`.
Root cause: source-reparse design (functions/classes travel as source, re-run on load).
**Status: documented & accepted.** `docs/pages/10-stdlib.md:1297` ("Security: never load a blob you do
not trust", :1310 explicitly equates it to Python `pickle`, directs to `json` across trust
boundaries). Not a defect — reinforce, do not "fix". Highest-impact property in scope, so flagged
prominently.

### F2 [LOW / CONFIRMED] JSON integer beyond int64 silently widens to Float (lossy); BigInt cannot be JSON-serialized
`stdlib_json.hpp:206-212`. `json.parse("9223372036854775808")` → `9.22337203685478e18` (precision
lost), and `json.parse("123456789012345678901234567890")` → a Float. Meanwhile `json.stringify(big)`
**hard-throws** `cannot serialize 'BigInt' to JSON` (`:308`). Asymmetric: parse lossily accepts, write
rejects — and Kirito *has* a lossless BigInt it declines to use on parse.
Root cause: deliberate ("mirroring dynamic languages", comment at `:207-208`). Documented/intentional;
noted only because it is a *silent* precision loss on the read path.

### F3 [INFO / CONFIRMED] JSON duplicate keys — last-wins silent merge
`stdlib_json.hpp:96` (`d.set` overwrites). `json.stringify(json.parse('{"a":1,"a":2}'))` → `{"a": 2}`.
Matches Python/JS; no diagnostic emitted. Standard behavior, informational.

### F4 [INFO / CONFIRMED] JSON number grammar is lenient vs RFC 8259
`stdlib_json.hpp:198-200`. Accepts leading zeros (`007`→7), trailing dot (`1.`→1.0), empty exponent.
Explicitly intentional and regression-guarded (comment `:194-197`). Strict-invalid structural inputs
(`{a:1}`, `[1,]`, `{"a":1,}`, `truee`, `{}x`, unterminated) are all correctly rejected.

### F5 [INFO / CONFIRMED] base64.decode is padding-lenient (but safe)
`stdlib_kimodules.hpp` base64 `decode` (`:703`). Accepts missing padding (`"abc"`→2 bytes) and ignores
ASCII whitespace (MIME/PEM). It DOES reject the always-invalid `len%4==1` ("lone trailing character",
`:729`), rejects non-canonical leftover bits (`:731`), rejects chars after padding (`:716`), and
rejects out-of-alphabet chars (`:718`). Matches lenient-safe decoders; not a defect.

---

## Verified CORRECT (with the vectors/checks used)

### Crypto/hash — `hashing.hpp`, `stdlib_hash.hpp`, `stdlib_crypto.hpp`
- **MD5**: `""`=d41d8cd9…427e, `"abc"`=90015098…7f72 — match.
- **SHA-1** `"abc"`=a9993e36…9d, **SHA-256** `""`=e3b0c442…b855 / `"abc"`=ba7816bf…15ad,
  **SHA-512** `"abc"`=ddaf35a1…ca49f, **SHA-384** `"abc"`=cb00753f…25a7 — all match FIPS vectors.
- **HMAC-SHA256**(key,"The quick brown fox…")=f7bc83f4…3cd8 — match.
- **PBKDF2-HMAC-SHA1** RFC 6070: c=1→0c60c80f…12a6, c=4096→4b007901…29c1 — match.
- **CRC-32**("123456789")=0xCBF43926, **Adler-32**("Wikipedia")=0x11E60398,
  **CRC-64/XZ**("123456789")=0x995DC9BBDF1939FA (signed −7395533204333446662) — all match.
- **PBKDF2 work factor**: `iterations` is a **required** param (no tiny default); `pbkdf2Raw` rejects
  <1 (`hashing.hpp:284`) and the binding rejects `<1` or `>2^32-1` (`stdlib_hash.hpp:78`) — no wrap to
  a 1-HMAC KDF. No insane default.
- **comparedigest** constant-time, length-mismatch folds without early-exit (`stdlib_hash.hpp:88-102`).
- **AES-256-GCM**: ciphertext + tag byte-for-byte identical to pyca/OpenSSL for fixed key/iv/aad
  (CT=3467b569…8c, TAG=da23c111…026f). Round-trips; tampered tag / truncated (8-byte) tag / wrong AAD
  all rejected (`aesdecrypt` requires exactly 16-byte tag, `stdlib_crypto.hpp:286`; auth-fail throw
  `:308`).
- **RSA** PKCS#1 sign/verify OK, tampered message → False; **RSA-OAEP(SHA-256)** encrypt/decrypt
  round-trips. **ECDSA** (prime256v1) sign/verify OK. **Key-type confusion** guard fires:
  `rsasign(ecPrivateKey)` throws (`requireKeyType`, `:138`).

### Serialization — `stdlib_serde.hpp`, `stdlib_serialize.hpp`, `stdlib_dump.hpp`
- Round-trips (both text + binary): scalars, nested List/Dict/Set, **cycles**, **shared refs**
  (identity preserved via `id()`), **Bytes**, class instances, closures/free-vars, `0.1`, and
  `DBL_MAX` (1.797…e308) exactly.
- **ASAN fuzz, 0 crashes / 0 hangs**: 1500 mutated binary blobs + 1200 mutated text blobs
  (bit-flips, truncation, extension, hostile object-count u32). Bounds are checked in `rebuild`
  (`checkId`), counts are input-bounded (`countToken`/`u32`), incremental `reserve` prevents the
  ~80× zeroed-alloc amplification, whole-token numeric parse rejects glued junk, trailing-data
  rejected. Corrupt input always yields a clean `KiritoError`, never a C++ escape.

### JSON — `stdlib_json.hpp`
- Round-trips floats via shortest-round-trip form; NaN/Infinity/-Infinity survive; control chars
  \u-escaped; UTF-16 surrogate pairs combined, unpaired surrogates → U+FFFD; depth guard (1000);
  cycle detection throws. Malformed inputs (see F4) rejected.

### Compression — `deflate.hpp`, `stdlib_zlib.hpp`, `stdlib_gzip.hpp`
- **Bidirectional interop with real Python zlib & gzip** (decode theirs, they decode ours). Empty,
  incompressible (2 KB random), and text all round-trip.
- **ASAN fuzz, 0 crashes / 0 hangs**: 2500 mutated zlib/gzip streams incl. crafted `78 9c` + garbage.
- Malformed handling: NLEN one's-complement checked (`deflate.hpp:288`), zlib FCHECK%31 / CINFO /
  FDICT rejected (`:366-373`), Adler-32 + gzip CRC-32/ISIZE verified, invalid length/distance symbols
  and "distance too far back" throw.
- **Decompression bomb bounded**: 300 MiB-of-zeros zlib (305 KB blob) → `decompressed data exceeds the
  size limit` (256 MiB cap), not OOM. gzip caps the AGGREGATE across concatenated members.

### Time — `stdlib_time.hpp`
- epoch 1234567890 → 2009-02-13T23:31:30, weekday 5 (Fri, 0=Sun); leap 2000-02-29; epoch round-trip
  exact; `make` rollover documented (month 13→next Jan, day 32→+1); year-0 OK; epoch
  999999999999999999 rejected (range). `strptime` rejects out-of-range fields (`2024-13-99`) and
  trailing junk (`2024-01-01XYZ`); good input parses. `sleep`/`datetime` guard NaN/Inf/overflow.

### Random — `stdlib_random.hpp`
- Same seed → identical stream (xoshiro & mt19937_64); different seeds differ; **checkpoint
  (`dump`) restore continues the exact stream**; `randombelow(10)` bias-free & in [0,10) over 1000
  draws (rejection sampling, `:393`); `randombytes(16)` len OK; CSPRNG-backed secure fns.

### io / path / sys
- **io**: `seek` rejects negative target and overflow (`stdlib_io.hpp:253-258`); `read(n)` clamps to
  bytes-available and chunk-reads non-seekable streams (no pre-alloc OOM, `:95-127`); BytesIO write
  overflow-safe (`:288`); files closed via destructor + `with`/`_exit_`; I/O on closed/wrong-mode file
  throws (no silent no-op).
- **path**: NUL in any filesystem path rejected (`requireNoNulPath`, `:87-91`); `join` absolute-reset
  semantics correct. (No path-traversal sandboxing — by design, a general FS API like `os.path`.)
- **sys**: NUL in argv/cwd rejected before syscall (`:50-54`, poison-NUL truncation bypass closed);
  env access serialized by a process-wide mutex (UAF/race guard); `exit` uses `_Exit` after flush to
  avoid the parallel-worker atexit deadlock. `sys.shell` runs `/bin/sh -c` (documented feature, not a
  vuln).

## Honest nulls
- Did not exercise X.509 `x509parse` against a real cert corpus, nor RSA/EC PEM import of malformed
  keys beyond the OpenSSL error path (OpenSSL owns that parsing; RAII deleters verified by reading).
- Crypto tested on the TLS-enabled release binary (`crypto.enabled == True`); the no-TLS build path
  (clear "requires KIRITO_ENABLE_TLS" throws) was read, not run.
- Fuzzing was mutation-based (thousands of iterations), not coverage-guided libFuzzer; no crash found
  but absence is not proof.
