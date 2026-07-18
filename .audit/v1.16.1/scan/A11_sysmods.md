# A11 — io/path/sys/time/hash/crypto/zlib/gzip (v1.16.1) [partial: hash + path directly audited]

Directly audited hash + path via a Known-Answer-Test golden (`spec_v1161_hash_path.ki`) — the ruleset's
"verify critical algorithms against independent implementations":
- md5("")/("abc"), sha1("abc"), sha256("")/("abc"), sha512("abc") vs reference vectors;
- HMAC-SHA256 vs **RFC 4231** test case 2; crc32("123456789")==0xCBF43926; adler32("Wikipedia");
  comparedigest; hash accepts Bytes == String.
- path join (+ absolute-reset), basename/dirname, splitext (multi-ext + no-ext), join()-zero-args throws.
Result: NO bugs.

## Coverage notes (NOT yet adversarially audited this round — candidates for a later batch)
- **pbkdf2** KAT (RFC 6018 vectors), sha384, crc64 — add KATs.
- **zlib/gzip** round-trip on Bytes vs String (same-type-out contract), interop with real gzip, empty
  input, corrupt-stream error clarity.
- **io** stream redirection (rebindable stdout/stderr/stdin), BytesIO cursor, binary mode Bytes I/O,
  `with`-context file, duck-typed write/read objects.
- **sys** createprocess/shell (argv vs shell, Bytes-in/binary-out, timeout kill), environ round-trip,
  platform/arch shape.
- **time** DateTime fields/iso/strptime/arithmetic/equality-by-instant/hash/serialize; monotonic/perf clocks.
- **crypto** (OpenSSL-gated) AES-GCM tag failure, RSA/EC sign+verify, X.509 parse — needs a TLS build.
