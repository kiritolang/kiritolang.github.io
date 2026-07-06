# v1.15 audit — zlib/DEFLATE, gzip, hash

Subsystem: `src/kirito/deflate.hpp`, `stdlib_zlib.hpp`, `stdlib_gzip.hpp`, `stdlib_hash.hpp`.
Probe binary: `./build-debug/ki`

## LOG
- Read all 4 source files + hashing.hpp + bytes.hpp helpers deeply.
- Verified KNOWN-ANSWER vectors (all PASS): md5/sha1/sha256 of ""/"abc"; adler32("")=1,
  adler32("abc")=0x024D0127; crc32("")=0, crc32("abc")=0x352441C2, crc32("123456789")=0xCBF43926;
  crc64/XZ("123456789")=0x995DC9BBDF1939FA (signed -7395533204333446662). All correct.
- Bytes with high bytes (0xFF,0x00,0xAB) hash byte-exactly (matches Python hashlib/zlib), NOT
  UTF-8-mangled. String-or-Bytes input handled correctly everywhere.
- RETURN-TYPE contract verified for EVERY function: String-in->String-out, Bytes-in->Bytes-out for
  zlib.compress/decompress/deflate/inflate, gzip.compress/decompress/gzip/gunzip; hashes always
  return String, checksums always Integer. Wrong-type input (Integer) throws cleanly.
- Round-trip PASS: empty / 1-byte / highly-repetitive / pseudo-binary for both String and Bytes,
  all three formats (zlib, gzip, raw deflate).
- INTEROP both directions PASS: our zlib/gzip/deflate output decompresses in Python; real Python
  zlib/gzip (incl. level-9, filename header, multi-member) decompresses in ours. Manual gzip with
  FEXTRA+FNAME+FCOMMENT+FHCRC decodes correctly.
- ADVERSARIAL: hand-crafted truncated/corrupt blobs (short zlib, bad CM, CINFO>7, bad FCHECK,
  FDICT preset dict, corrupt adler/crc/isize, bad gzip magic/CM, bad stored-block NLEN complement,
  invalid BTYPE=3, invalid length/distance symbols) ALL throw clean catchable KiritoErrors — no
  crash, no garbage-accept.
- FUZZ: 4500 random/semi-structured blobs through zlib.inflate — 0 crashes, 0 hangs, only clean
  DeflateError messages (distance-too-far-back, invalid Huffman code, bad dynamic lengths, etc.).
  Traced Huffman::build/decode: index bound `index+(code-first) < symbols.size()` holds even for
  over-subscribed/incomplete trees -> no OOB. dsym>=30 / lensym>=29 guarded. Stored-block bounds
  checked. No infinite loops (BitReader throws at EOF).
- ZIP-BOMB caps FIRE (not OOM): 300MB-of-zeros zlib stream and 12x30MB multi-member gzip both throw
  "decompressed data exceeds the size limit"; gzip aggregate budget shrinks across members correctly.
- hash.hash(x) works on hashable values; unhashable (List) throws "unhashable type 'List'".

Conclusion: this subsystem is very well hardened. Only two minor SUSPECT/LOW items below; no
HIGH/MED confirmed bugs.

## FINDINGS
