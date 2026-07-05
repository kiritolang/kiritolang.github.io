# Doc-vs-impl audit: `hash`, `zlib`, `gzip` modules

Verified against `docs/pages/10-stdlib.md` (`## hash`, `## zlib`, `## gzip`) and the impls
`src/kirito/stdlib_hash.hpp`, `stdlib_zlib.hpp`, `stdlib_gzip.hpp` (+ `deflate.hpp`, `hashing.hpp`).
Runner: `build-debug/ki`. Tests: `tools/tests/scripts/verify_hash.ki` (`OK hash`),
`tools/tests/scripts/verify_compress.ki` (`OK compress`).

## Discrepancies

**None.** Every documented function, return type, alias, and error path matches the implementation.

## Pinned current behaviour (known-good vectors + observed messages)

### hash
- Digests (lowercase hex, confirmed vectors): `md5("")=d41d8cd98f00b204e9800998ecf8427e`,
  `md5("abc")=900150983cd24fb0d6963f7d28e17f72`,
  `sha1("")=da39a3ee5e6b4b0d3255bfef95601890afd80709`,
  `sha1("abc")=a9993e364706816aba3e25717850c26c9cd0d89d`,
  `sha256("")=e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`,
  `sha256("abc")=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`.
- Checksums (Integer): `adler32("")=1`, `adler32("a")=6422626`, `adler32("Wikipedia")=300286872`;
  `crc32("")=0`, `crc32("123456789")=3421780262` (0xCBF43926); `crc64("")=0`,
  `crc64("123456789")=-7395533204333446662` (0x995DC9BBDF1939FA reinterpreted signed, per CRC-64/XZ).
- Every byte-data fn accepts String OR Bytes and gives identical results (`hash.md5(s)==hash.md5(s.encode())`).
- `hash(value)`: Integer is identity (`hash(42)==42`); String/Bytes content-based & deterministic;
  Bool/None/Float stable; user-class `_hash_` composes (`hash.hash(Email("a@b"))==hash.hash("a@b")`).
- HARDENING: `hash([..])` / `hash({..})` / `hash({set})` throw `unhashable type '<Name>'`.
  Digests/checksums on a non String/Bytes arg (Integer/List/None) throw (arg-view rejection).

### zlib (RFC 1950 stream + raw DEFLATE)
- `compress`/`decompress`/`deflate`/`inflate` all round-trip; **result type == input type**
  (String→String, Bytes→Bytes), including empty, unicode, large/repetitive (compresses smaller),
  and arbitrary binary Bytes (byte-exact). Raw `deflate` output ≠ `compress` output (no header/trailer).
- HARDENING (messages prefixed `zlib:`): garbage → `zlib: unsupported zlib compression method`;
  raw-inflate garbage → `zlib: invalid block type`; empty → `zlib: zlib data too short`;
  truncated → `zlib: unexpected end of deflate stream`.

### gzip (RFC 1952 container)
- `compress`/`decompress` + aliases `gzip`/`gunzip` round-trip; result type == input type.
  Stream starts with magic `0x1f 0x8b 0x08`. Concatenated members (`a.gz + b.gz`) all decode & join.
- Cross-format: `zlib.compress(x) != gzip.compress(x)`; each rejects the other's stream.
- HARDENING (messages prefixed `gzip:`): non-gzip → `gzip: bad magic (not a gzip stream)`;
  `< 18` bytes → `gzip: stream too short`; truncated body → `gzip: truncated stream`;
  flipped body byte → `gzip: CRC-32 mismatch (corrupt stream)`. (ISIZE trailer also verified in impl.)

## Coverage / assertion counts
- `verify_hash.ki` — ~46 assertions: md5/sha1/sha256 vectors + length/lowercase, adler32/crc32/crc64
  known values + type, String==Bytes equivalence (all 6 fns), binary Bytes, `hash(value)` identity/
  determinism/`_hash_` composition, unhashable + non-String/Bytes rejection.
- `verify_compress.ki` — ~48 assertions: zlib & raw round-trips w/ type preservation, empty/unicode/
  large/binary edges, gzip round-trip + aliases + magic + concatenated members, cross-format
  rejection, and full garbage/truncated/corrupt hardening for both modules.
