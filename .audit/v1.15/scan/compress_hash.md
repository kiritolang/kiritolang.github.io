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

### F1 [LOW/SUSPECT] gzip.decompress rejects trailing NUL padding that zlib/gzip(1)/Python tolerate
- where: src/kirito/stdlib_gzip.hpp:47-56 (the multi-member loop + "trailing data after the last member" throw)
- repro: gzip.decompress(gzip.compress("hello") + Bytes([0,0,0,0]))  -> THROW
  "gzip: trailing data after the last member". Python's gzip.decompress(gc + b"\x00\x00\x00\x00")
  returns b'hello' (NUL padding ignored); zlib's gzip C reader and gzip(1) also skip trailing
  zero padding (common with fixed-block/record storage). Non-NUL trailing garbage is correctly
  rejected by both us and Python.
- actual: any trailing byte (incl. NUL padding) after the last valid member throws.
  expected (interop): trailing all-zero padding should be ignored (as libz/gzip(1)/Python do),
  while non-zero trailing junk stays an error.
- fix idea: after a member completes, if the remaining bytes are all 0x00, treat them as padding
  and stop (return result) instead of attempting another member / throwing. Keep the strict
  rejection for non-zero trailing bytes. This is arguably a deliberate strictness choice (the code
  comment says "reject, not silently ignore"), hence SUSPECT — flagging only as a real-world
  interop gap for a genuinely-valid .gz.

### F2 [LOW/SUSPECT] deflate::compress uses `int` for window indices -> UB for inputs > 2 GiB
- where: src/kirito/deflate.hpp:91,121,138-142,150-151 (`std::vector<int> head/prev`,
  `head[h] = static_cast<int>(i)`, `j = prev[j]`, `i - static_cast<size_t>(j)`)
- repro: not practically reachable from Kirito — needs a >2^31-byte input String/Bytes in memory.
  No .ki repro produced (would need multi-GB allocation).
- actual: for i > INT_MAX, `static_cast<int>(i)` overflows (signed overflow UB) and the hash-chain
  indices become negative/garbage. Decompression is capped at 256 MiB (kMaxInflateOut) but
  COMPRESSION input has no size guard.
- expected: either cap compressor input size (throw like the other resource guards) or use
  size_t/int64 for the chain indices.
- fix idea: change head/prev to `std::vector<std::ptrdiff_t>` (or int64) and the casts accordingly,
  or add an input-size guard. Theoretical (memory-bound), hence LOW/SUSPECT.

## NON-FINDINGS (checked, ruled out)
- Huffman over-subscription / incomplete trees: no OOB (index bound proven), decode throws cleanly.
- Fixed dist codes 30/31, dynamic dsym 30/31, length sym >=29: all rejected.
- Stored-block LEN/NLEN complement validated; truncation checked; append bounds checked.
- zlib header: CM!=8, CINFO>7, FDICT, FCHECK%31, adler mismatch all validated.
- gzip header: magic, CM, FEXTRA/FNAME/FCOMMENT/FHCRC skipping bounds-checked; CRC-32 + ISIZE
  both verified; truncation at every stage throws.
- Zip-bomb: single-stream 256 MiB cap + gzip multi-member shrinking aggregate budget both enforced.
- Return-type contract, high-byte Bytes hashing, all KAT vectors: correct.
