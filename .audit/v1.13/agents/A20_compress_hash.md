# A20 — compress + hash audit (deflate / zlib / gzip / hash)

**Agent:** A20
**Area:** self-contained DEFLATE/INFLATE (RFC 1951), zlib container (RFC 1950), gzip container (RFC 1952),
digests/checksums (md5/sha1/sha256/adler32/crc32/crc64).
**Files audited (read-only):**
- `src/kirito/deflate.hpp` (adler32, crc32, BitWriter, compress, BitReader, Huffman, inflateBlock, inflateImpl, inflate, zlibCompress, zlibDecompress)
- `src/kirito/stdlib_zlib.hpp` (ZlibModule: compress/decompress/deflate/inflate)
- `src/kirito/stdlib_gzip.hpp` (GzipModule: compress/decompress, aliases gzip/gunzip)
- `src/kirito/stdlib_hash.hpp` (HashModule: md5/sha1/sha256/adler32/crc32/crc64/hash) + `src/kirito/hashing.hpp`
- Tests: `tools/tests/unit/test_zlib.cpp`, `test_hash.cpp`, `test_compress_hash_deep.cpp`, `test_audit_hardening.cpp` (zlib FDICT/FCHECK), `.ki` scripts `probe_compress_fuzz.ki`, `spec_gzip.ki`, `deep_compress.ki`.

**Method:** static reasoning only (no build/run), adversarial focus on the INFLATE decoder (it consumes
untrusted `net.get().content` / files). Bit-reader, Huffman decode, block dispatch, gzip header-flag
parsing, checksum verification, and integer widths traced by hand.

**Headline:** No memory-safety bug found — the Huffman decoder is provably in-bounds even for corrupt
tables (see A20-1 analysis), the bit reader throws on EOF, distance-too-far-back is guarded, stored-block
NLEN is validated, and the zip-bomb ceiling exists. The findings are dominated by **coverage gaps**: the
entire **dynamic-Huffman (BTYPE=2) decode path is untested** because Kirito's own compressor only emits
fixed-Huffman blocks and no test feeds a real zlib/gzip fixture; plus a **missing bomb-guard test** and
**no rejection of incomplete/over-subscribed Huffman code tables** (an RFC/zlib interop divergence).

Severity legend: **security** (crash/OOB/hang/OOM on untrusted input) > **correctness** > **robustness/interop** > **coverage-gap** > **quality/perf**.

---

## Confirmed defects / divergences

### A20-1: Dynamic-Huffman (BTYPE=2) decode path is effectively untested — Kirito's compressor never emits it
- **severity:** coverage-gap (HIGH value — a large untested code surface that decodes untrusted input)
- **location:** `deflate.hpp` `inflateImpl` `case 2:` (lines 295-327), the dynamic-table build/decode; the whole test suite.
- **category:** test coverage / latent-bug risk
- **description:** `compress()` (deflate.hpp:83) *always* writes `BFINAL=1, BTYPE=01` (fixed Huffman) — it has no dynamic-Huffman or stored-block encoder. Every round-trip test (`test_zlib.cpp`, `test_compress_hash_deep.cpp`, `probe_compress_fuzz.ki`, `spec_gzip.ki`, `deep_compress.ki`, and even `test_net.cpp:80` / `test_net_tls.cpp:142` which hand-build gzip headers) inflates only streams *produced by this same compressor*, i.e. only `case 1`. The only hand-crafted deflate bytes in the suite are **stored blocks** (`test_zlib.cpp:64-67`, BTYPE=00). Result: the ~35 lines that parse HLIT/HDIST/HCLEN, decode the code-length Huffman, expand run-length codes 16/17/18, split lit/dist tables and decode dynamic literals/distances are **never executed by any test**. This is the most complex, most attacker-reachable code in the module (real-world gzip/zlib almost always uses dynamic blocks), yet has zero direct coverage.
- **failure-scenario:** A regression in the dynamic-table decode (off-by-one in the HLIT/HDIST split, a wrong `order[]` entry, a broken repeat-code) ships green. Worse, a crash/OOB introduced there would not be caught.
- **proposed-test:** Embed a handful of **real zlib and real gzip byte fixtures produced by an external tool** (python `zlib.compress`/`gzip.compress` of representative text — these emit dynamic blocks) as `Bytes([...])` literals and assert `zlib.decompress` / `gzip.decompress` recover them. Add a couple of *hand-crafted minimal dynamic blocks* (a tiny dynamic-Huffman block with a code-16/17/18 repeat) to exercise the run-length path directly. Also a fixture that mixes multiple block types (stored + fixed + dynamic) in one stream.
- **proposed-fix:** none to source; add the fixtures above.
- **confidence:** high (verified compress() only writes BTYPE=01 and grepped every test input).

### A20-2: Incomplete / over-subscribed Huffman code tables are accepted, not rejected (RFC 1951 / zlib divergence)
- **severity:** robustness/interop (NOT memory-unsafe — see analysis)
- **location:** `deflate.hpp` `Huffman::build` (197-206); callers at 302, 324-325.
- **category:** malformed-input handling / interop
- **description:** `build()` never checks that the supplied code lengths form a *complete* canonical Huffman tree (Kraft sum == 1). Real zlib rejects an **incomplete** literal/length or code-length tree ("incomplete dynamic bit lengths tree") and an **over-subscribed** one ("oversubscribed literal/length tree") up front. Kirito instead builds whatever table it is given and only errors *if and when* an undefined bit pattern is decoded (`decode()` throws "invalid Huffman code") — or silently decodes a wrong symbol for an over-subscribed table. The one legitimate RFC exception (a single distance code / empty distance tree) is also not special-cased. **Memory safety holds regardless:** in `decode()`, `index + (code - first)` is bounded because `code - first < count` and `Σcount ≤ #nonzero-lengths ≤ symbols.size()`, so no OOB read even for a maximally corrupt table (I traced this explicitly). So this is a *cleanliness/interop* divergence, not an exploit: some streams zlib rejects are accepted (possibly yielding different output before erroring).
- **failure-scenario:** A crafted dynamic block with an incomplete literal tree decodes partially and returns truncated data + a mid-stream "invalid Huffman code" rather than an immediate "incomplete code" — Kirito accepts inputs a conformant decoder rejects, so a `.gz` that `gunzip` refuses may "half-decode" here.
- **proposed-test:** Feed a hand-crafted dynamic block whose literal code-length table is incomplete (Kraft sum < 1) and assert a clean throw; feed an over-subscribed code-length table (Kraft sum > 1) and assert a clean throw. (Requires the fix to actually reject them.)
- **proposed-fix:** In `build()` (or the two dynamic callers), compute the Kraft sum from `counts[]` and throw `DeflateError("incomplete/oversubscribed Huffman code")` when `left != 0` after `for(len=1..maxBits) left = (left<<1) - counts[len]; if(left<0) oversubscribed;` — allow the RFC single-distance-code exception.
- **confidence:** high (behaviour is clear from the code; memory-safety verified).

### A20-3: No test asserts the decompression-bomb guard (`kMaxInflateOut`) actually fires
- **severity:** coverage-gap (security-relevant — the guard is the sole defense against OOM on untrusted `net.get().content`)
- **location:** `deflate.hpp:224` `kMaxInflateOut`, checks at 229/271/286; gzip shrinking budget `stdlib_gzip.hpp:73-76`.
- **category:** test coverage
- **description:** The 256 MiB output ceiling and the gzip aggregate-budget logic (A16-1) are security-critical but **no test ever drives output past the cap** to confirm the throw. The largest test payloads are ~1.5 MB (`test_compress_hash_deep.cpp:268-288`). A crafted highly-repetitive fixed-Huffman stream (or a multi-member gzip of many small members) that would exceed 256 MiB is never exercised, so a regression that removes/miscomputes the guard ships green.
- **failure-scenario:** Someone "optimizes" the per-symbol `if (out.size() > maxOut)` check out of the hot loop, or the gzip shrinking-budget math regresses to per-member — an actual zip bomb then OOMs the process, and no test catches it.
- **proposed-test:** Build a small stream that inflates to just over a *small injected* ceiling — best via a test hook or by asserting the message on a crafted stored/fixed stream sized to exceed a lowered cap; alternatively assert a multi-member gzip whose aggregate would exceed 256 MiB throws "exceeds the size limit". At minimum, a unit test on `deflate::inflateImpl(view, smallMaxOut, &c)` with a stream whose output > `smallMaxOut` asserting the throw (this exercises the exact guard without allocating 256 MiB).
- **proposed-fix:** none to source; add the guard test (ideally exercising `inflateImpl` with a small `maxOut` directly so it's cheap).
- **confidence:** high.

---

## Gaps / weaker spots (comprehensive)

### A20-4: Malformed-INFLATE input space is thinly covered
- **severity:** coverage-gap
- **location:** `inflateBlock` (226-246), `inflateImpl` (266-335).
- **description:** Existing malformed coverage: bad block type / truncated (`"\xff\xff\xff"`), stored-block NLEN corruption (test_zlib.cpp:66), too-short zlib, bad zlib body, FDICT/FCHECK (test_audit_hardening.cpp:196), CINFO>7, gzip magic/CM/FEXTRA/FNAME/trailer truncation. **Untested** decoder edges (all are *handled* in code, but nothing proves it): (a) invalid distance symbol 30/31 in a dynamic table (`if (dsym >= 30) throw`, line 239); (b) invalid length symbol 286/287 → sym-257≥29 (`if (sym >= 29) throw`, 236); (c) "distance too far back" (241) — a back-reference distance exceeding current output; (d) code-16 repeat with empty `lengths` (309, "invalid repeat"); (e) dynamic length count mismatch (`bad dynamic lengths`, 321); (f) HLIT/HDIST/HCLEN boundary values (min/max: hlit=257/288, hdist=1/32, hclen=4/19); (g) a truncated dynamic header (EOF mid code-length table); (h) a fixed/dynamic block that never emits EOB (must terminate via EOF-throw, not hang). None of these have a test.
- **failure-scenario:** A regression in any of these guards (e.g. dropping the `dsym >= 30` bound, which would index `kDistBase[30]` OOB) ships undetected.
- **proposed-test:** A crafted-bytes table driving `zlib.inflate`/`deflate::inflateImpl` for each of (a)-(h), asserting a specific catchable error and no crash/hang.
- **confidence:** high.

### A20-5: No real-world / interop fixtures (external zlib, gzip(1), PNG zlib streams)
- **severity:** coverage-gap / interop
- **location:** whole module.
- **description:** Comments assert "real zlib-produced streams decompress correctly" (deflate.hpp:9) and gzip is "`gzip(1)`-compatible", but there is **no fixture from an external producer** anywhere in the suite — every decoded stream came from Kirito's own encoder. Missing interop cases: a `gzip(1)` stream with a real MTIME + FNAME + OS byte; a `.gz` with the FTEXT flag; a zlib stream at a non-default compression level (different CMF/FLG check bytes, dynamic blocks); a PNG-style zlib IDAT chunk. Also the *encode* side is unverified against real decoders (no test decompresses Kirito's `gzip.compress` output with an external `gunzip`).
- **proposed-test:** Add a few externally-produced `Bytes([...])` fixtures (python `gzip`/`zlib`, `gzip(1)`) and assert exact recovery; optionally a smoke step that pipes `gzip.compress` output to system `gunzip` in a script.
- **confidence:** high.

### A20-6: Checksums recomputed bit-by-bit over the full (up-to-256 MiB) decompressed output — CPU-DoS amplification
- **severity:** quality/perf (low security)
- **location:** `zlibDecompress` adler32 over `out` (375); gzip `crc32(out)` (80). Both `adler32` (24-31) and `crc32` (34-41) are table-free, per-bit loops.
- **description:** After inflating up to `kMaxInflateOut` (256 MiB), the verifier re-scans the whole output with a **bit-by-bit** CRC-32 (8 inner iterations/byte ⇒ ~2 billion ops for a near-cap bomb) and a per-byte modulo adler32 (no NMAX 5552-batching ⇒ a `% 65521` twice per byte). An attacker can push a stream to just under the cap and force seconds of CPU per `decompress` call. Bounded (not unbounded), so low severity, but a cheap amplification vector on untrusted input.
- **failure-scenario:** Many concurrent `net.get(...).content` → `gzip.decompress` of near-256 MiB bombs pin CPU.
- **proposed-fix:** Use a table-driven CRC-32 (a `static const` 256-entry table computed once — still no *mutable* global) and NMAX-batch adler32 (defer the `%` every 5552 bytes). Both are standard and eliminate ~90% of the cost. Optional: consider a lower default inflate ceiling for the network path.
- **confidence:** medium (perf-only; correctness unaffected).

### A20-7: `zlibDecompress` does not verify inflate consumed the body up to the trailer
- **severity:** robustness (low)
- **location:** `zlibDecompress` (355-377): `body = in.substr(2, in.size()-6); out = inflate(body);` then adler read from the fixed last 4 bytes.
- **description:** `inflate()` stops at the final block and silently ignores trailing bytes in `body`. The trailer is assumed to be exactly the last 4 bytes. So interior junk between the deflate stream's end and the last 4 bytes is ignored; corruption is only caught if it perturbs those last 4 bytes (adler mismatch). Unlike gzip (which tracks `consumed` and locates the trailer precisely, stdlib_gzip.hpp:69-77), zlib doesn't confirm the deflate stream length. Practically harmless (a mismatched adler still throws in the common case), but it accepts some non-canonical layouts and yields a generic "checksum mismatch" rather than a precise "trailing data" error.
- **proposed-fix:** Use `inflateImpl(std::string_view(in).substr(2, in.size()-6), kMaxInflateOut, &consumed)` and require `2 + consumed == in.size() - 4` before checking adler (or reject trailing data explicitly), mirroring gzip.
- **confidence:** medium.

### A20-8: gzip FHCRC (header CRC-16) is skipped, never verified
- **severity:** robustness/interop (low; documented)
- **location:** `stdlib_gzip.hpp:67` `if (flg & 0x02) pos += 2;`
- **description:** The 2-byte FHCRC is advanced over but not checked against a CRC-16 of the header. RFC 1952 makes it optional and `gzip(1)` is lenient, so this is acceptable, but a corrupt header that a strict decoder would catch via FHCRC passes here (and may then fail later with a less precise error). Tested only that it's *skipped* (test_compress_hash_deep.cpp:73). Note as a conscious spec gap.
- **proposed-fix:** optional — verify FHCRC against `crc32(header) & 0xFFFF` low word when the flag is set (that is what gzip uses), throwing "gzip: header CRC mismatch".
- **confidence:** high (behaviour clear; low impact).

### A20-9: Encoder emits neither stored nor dynamic blocks; incompressible data grows and the stored-encode path is nonexistent
- **severity:** quality (low)
- **location:** `deflate.hpp:83-160` `compress()`.
- **description:** `compress()` only produces one fixed-Huffman block. It therefore (a) *grows* incompressible input a few percent instead of falling back to a stored block (acknowledged in test_zlib.cpp:140 `pct() > 99.0`), and (b) produces gzip/zlib output larger and structurally simpler than real tools. Not a bug, but it is *why* A20-1/A20-5 exist: the decoder's dynamic and stored *encode* counterparts are never fed by self-produced streams. Minor: could hurt the `net` upload path / on-disk `.gz` size.
- **proposed-fix:** optional — add a stored-block fallback when a block would expand, and/or a dynamic-Huffman encoder. Low priority.
- **confidence:** high.

### A20-10: `inflate` (raw) / `zlib.inflate` silently ignore trailing bytes and have no output-consumption contract
- **severity:** robustness (very low — standard for raw inflate)
- **location:** `deflate.hpp:339` `inflate`; exposed as `zlib.inflate` (stdlib_zlib.hpp:40).
- **description:** Raw inflate legitimately stops at BFINAL and returns; trailing junk is not an error (matches zlib's raw mode). Noted for completeness — no fix needed, but worth a test asserting `zlib.inflate(deflate + junk)` still returns the payload (documents the intentional behaviour) and that a *truncated* raw stream throws "unexpected end".
- **confidence:** high.

---

## Verified-safe (explicitly checked, no defect)

- **Huffman decode is in-bounds for any corrupt table** (A20-2 analysis): `symbols[index + (code-first)]` ≤ `symbols.size()` always.
- **No infinite loop:** every block path monotonically consumes input bits; EOF throws "unexpected end". Stored blocks of LEN=0 still advance `pos_` by 4/iteration. Dynamic length-build loop is bounded (≤ ~320+138 pushes then `bad dynamic lengths`).
- **Stored block:** `p+4`/`p+len`/`out.size()+len` bounds all pre-checked; NLEN = ~LEN validated (A16-2). `out.append(in, p, len)` in range.
- **gzip header parsing:** every `data[pos]` access is `pos < data.size()`-guarded; an overrunning FEXTRA/FNAME is caught by the `pos + 8 > data.size()` check before the `std::string_view::substr(pos)` (which is therefore never called with `pos > size`, avoiding `std::out_of_range`). `off + 18 > size` gates all fixed-header reads. Trailer reads `t+8`-guarded.
- **Dynamic symbol bounds:** `dsym >= 30` and `sym-257 >= 29` guards prevent OOB into `kDistBase`/`kLenBase`; fixed-dist codes 30/31 throw "invalid Huffman code" cleanly.
- **Distance guard:** `d > out.size()` → "distance too far back" prevents OOB back-reference read.
- **Integer widths:** stored LEN/NLEN uint16; `~len` narrowed correctly; xlen/isize/pos additions cannot wrap size_t (bounded by data.size()+~65543); crc/isize trailer built with explicit `uint32_t` casts (no signed-shift UB). Consistent with the repo's `-Wconversion`-clean claim.
- **gzip `consumed`/trailer alignment:** `alignByte(); bytePos()` yields the correct rounded-up byte count (partial final byte already counted in `pos_`).
- **Multi-member gzip bomb budget** (A16-1) shrinks correctly; empty member with budget 0 returns cleanly.
- **hash/hashing:** md5/sha1/sha256 padding (`% 64 != 56` + 8-byte length) correct for empty / block-boundary (55/56/63/64) inputs (well tested); `M[]`/`w[]` fully initialized (no uninit read); crc64/crc32/adler32 sign handling (uint→int64 zero-extend) non-negative for 32-bit, documented negative for crc64. `hash()` builtin dispatches `_hash_`, rejects unhashable with the standard message.
- **String-vs-Bytes wrapper** (`argStringOrBytes`/`makeStringOrBytes`) is DRY-shared across zlib/gzip/hash codecs; DeflateError→KiritoError translation preserves the `gzip:`/`zlib:` prefix; empty Bytes round-trips (tested).

---

## Summary

- **Findings:** 10 (0 memory-safety bugs; 1 interop/robustness divergence [A20-2]; 6 coverage gaps; 3 quality/perf). No crash/OOB/hang/OOM reachable from untrusted input was found — the decoder is memory-safe by construction.
- **Top 5:**
  1. **A20-1** — the entire **dynamic-Huffman (BTYPE=2) decode path is untested**: Kirito's compressor only emits fixed-Huffman, and no test feeds a real zlib/gzip fixture, so the most complex attacker-reachable decode code has zero coverage.
  2. **A20-2** — **incomplete/over-subscribed Huffman tables are accepted, not rejected** (RFC/zlib divergence); memory-safe but non-conformant, should throw up front.
  3. **A20-3** — **no test exercises the 256 MiB zip-bomb guard**; the sole OOM defense on untrusted input could regress silently.
  4. **A20-4** — thin malformed-INFLATE coverage: invalid dist symbol 30/31, length symbol 286/287, distance-too-far-back, code-16-empty repeat, HLIT/HDIST edges, truncated dynamic header — all handled but untested.
  5. **A20-6** — checksum verification uses bit-by-bit CRC-32 / unbatched adler32 over the full ≤256 MiB output ⇒ CPU-DoS amplification near the cap (fix: table-driven CRC + NMAX adler batching).
