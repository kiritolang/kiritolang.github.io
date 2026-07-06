# A17 — zlib + gzip + hash + regex audit (v1.14)

Status: COMPLETE

Subsystem files (all READ-ONLY here):
- src/kirito/stdlib_zlib.hpp, stdlib_gzip.hpp, stdlib_hash.hpp
- src/kirito/regex_engine.hpp, stdlib_regex.hpp
- src/kirito/deflate.hpp (core INFLATE/DEFLATE), hashing.hpp (md5/sha1/sha256)

Binary used to confirm: /home/user/kiritolang.github.io/build-debug/ki
Method: crafted real Python zlib/gzip + gzip(1) CLI fixtures (dynamic-Huffman) and
adversarial `.ki` probes; compared regex behavior byte-for-byte against Python `re`.

## What I verified GREEN (no defect)
- **Dynamic-Huffman interop**: real `zlib.compress`, `gzip.compress`, and `gzip -9` (CLI)
  streams over prose/json/random/repetitive/empty/1-byte all decompress bit-exactly
  (sha256-verified). The BTYPE=2 decode path (A20-1 gap) is CORRECT — just untested (A17-3).
- **gzip header flags** FEXTRA+FNAME+FCOMMENT+FHCRC (crafted, all four set) decode correctly;
  multi-member (`cat a.gz b.gz`) → concatenated bodies.
- **Corruption throws cleanly** (no crash/OOB/OOM): bad CRC-32, bad ISIZE, truncated header,
  non-gzip garbage, truncated zlib, bad Adler-32, garbage deflate body — each a catchable
  `KiritoError` with a precise message.
- **Decompression-bomb guard fires**: 300 MiB zlib bomb, 300 MiB gzip bomb, and a 600 MiB
  multi-member gzip bomb all throw "decompressed data exceeds the size limit" (no OOM).
- **hash known-answer vectors** all correct: md5/sha1/sha256 for ""/"abc"/1 MB/unicode;
  adler32/crc32 known values; crc64/XZ check value for "123456789" = 0x995DC9BBDF1939FA
  (= -7395533204333446662 signed). String vs Bytes → identical digest.
- **regex conformance vs Python `re`**: 19/19 cases identical, incl. zero-width findall,
  empty-pattern split, capture-group split, Python-3.7 empty-capable split semantics,
  non-greedy, MULTILINE `^`/`$`, DOTALL, `\b`, IGNORECASE, named groups, `\g<0>`.
- **Unicode**: `.` spans one code point, IGNORECASE folds Latin (é/É), class matches multi-byte
  code points, positions are code-point indices.
- **Rejection paths clean** (26 probes): backrefs, look-ahead/behind, named backrefs, dup group
  name, `{3,1}`, `{2000}`, `[z-a]`, leading `*`/`+`/`?`, `[]`, unterminated class, unbalanced
  parens, trailing `\`, empty group name, `\uD800` surrogate, `\U00110000`, incomplete `\x`,
  digit-start group name, 3000-deep nesting (native-stack-safe), `(a{1000}){1000}` → program cap.
- **Linear-time holds**: `(a+)+b`, `(a|a)*b`, `(a*)*b`, `(x+x+)+y` on 1000-char non-matching
  input all <6 ms. `endpos` correctly anchors `$`; `pos`/`endpos`=None-safe & clamped
  (A21-2/A21-4 from v1.13 are FIXED via `resolvePosEndpos`). Zero-width `sub` matches Python.

## Findings

### A17-1: Regex capture-group DoS constant is still practically exploitable — the numGroups≤1000 cap is set far too high to be an effective guard
- **severity:** MEDIUM (security — untrusted-pattern linear-time is a stated security property)
- **location:** `regex_engine.hpp` `run()` (:641-696) + `addThread` per-thread `caps` copy (:592-625); the only guards are compile-time (`numGroups>1000` :529, program `>=200000` :428, depth `>2000` :123). There is **no runtime step/time/work budget**.
- **category:** DoS / weak-spot / performance
- **description:** The Pike VM is O(text × program × numGroups) because every thread copies a `2*(numGroups+1)`-int capture vector on each split/save. The individual caps are bounded but their **product is not**. Measured on the shipped debug binary:
  - `(a?)`×500 (500 groups, ~2 KB program) via `findall` over a 10 000-char input → **20.8 seconds** wall-clock.
  - `(.)`×200 via `search` over a 20 000-char input → **3.3 seconds**.
  - `(a)(b)` `findall` over 10 000 chars → 65 ms (baseline).
  A 500-group pattern is *half* the allowed cap; 1000 groups over a longer input scales to minutes. The v1.13 mitigation (cap groups at 1000) does not prevent the DoS — it only bounds it at an unusably large ceiling. Reachable from any `regex.compile`/`sub`/`findall` on an attacker-supplied pattern **and** subject.
- **failure-scenario:** A service that runs a user-supplied regex (log filter, search box) against user text hangs a whole worker VM for tens of seconds per request.
- **proposed-test:** a spec that compiles `("(a?)"*400)` and asserts `findall` over a 5 000-char string completes under ~200 ms — fails today, passes once a budget/lower-cap lands.
- **proposed-fix:** add a runtime work budget in `run()` (a counter of thread-steps or `caps`-copies that throws a catchable "regex match budget exceeded" past a ceiling), mirroring `kMaxInflateOut` for inflate; and/or lower the numGroups cap to ~100. A step budget is the robust fix because program size × input blows up independent of groups.
- **confidence:** HIGH (reproduced, timed).

### A17-2: `regex.sub` with a negative `count` replaces ALL matches; Python replaces NONE
- **severity:** LOW (correctness divergence, unusual input)
- **location:** `stdlib_regex.hpp:325` (`int count = ... asInt("count")`) and `:336` (`allMatches(R.prog, text, count > 0 ? count : -1)`).
- **category:** bug / semantic divergence
- **description:** `count > 0 ? count : -1` maps both `0` and any negative value to "unlimited". Python's `re.sub(p, r, s, count=-1)` performs **0** replacements (`re.sub('a','X','aaaa',-1)` → `'aaaa'`); Kirito returns `'XXXX'`. (The int64→int narrowing of a huge positive count, A21-3, also folds into this branch but coincidentally lands on "replace all" too.)
- **failure-scenario:** code that passes a computed count which underflows negative silently replaces everything instead of nothing.
- **proposed-test:** `assert re.sub("a","X","aaaa",-1) == "aaaa"` (after fix).
- **proposed-fix:** treat negative count as 0 replacements: keep `0` == "all", but map `count < 0` to a 0-replacement short-circuit — matching Python.
- **confidence:** HIGH (reproduced both sides).

### A17-3: Dynamic-Huffman / real-tool interop decode has no test fixture — the entire compress suite decodes only Kirito's own fixed-Huffman encoder output
- **severity:** coverage-gap (HIGH value — a large untrusted-input decode surface with zero external-producer coverage). Refines A20-1/A20-5, still open in v1.14.
- **location:** `deflate.hpp` `inflateImpl` `case 2:` (:295-327 dynamic-table build/decode) and the whole `zlib`/`gzip` decode surface; tests in `tools/tests/unit/test_zlib.cpp`, `test_compress_hash_deep.cpp` produce every stream via `zlib.compress`/`gzip.compress`, which only emit **fixed-Huffman (BTYPE=1)** blocks. gzip flag tests splice flags onto self-encoded (fixed-Huffman) bodies.
- **category:** test coverage / latent-regression risk
- **description:** Kirito's compressor never emits dynamic-Huffman, so the dynamic decode path (canonical-table build, code-length RLE symbols 16/17/18, HLIT/HDIST/HCLEN handling) is exercised by NO test. My probe proves it is presently CORRECT against Python `zlib`/`gzip` (level 9, dynamic) and real `gzip -9` CLI output, but a future refactor could silently break decoding of every real-world `.gz`/zlib/PNG-IDAT stream without a red test.
- **failure-scenario:** a regression in the dynamic-table code (e.g. an off-by-one in the code-length RLE) breaks `net.get(url).content` → `gzip.decompress` on real HTTP `Content-Encoding: gzip`, undetected by CI.
- **proposed-test:** embed a handful of byte-literal fixtures produced by real `zlib`/`gzip -9` (as `Bytes([...])` in a `.ki`, or a C++ `std::string`), assert `decompress` yields the known payload — locking in the interop the comments already claim.
- **confidence:** HIGH (probe passed; grep confirms no external fixture exists).

### A17-4: The decompression-bomb guard (`kMaxInflateOut`) firing is not asserted by any test
- **severity:** coverage-gap (security-relevant — the sole OOM defense on untrusted `net.get().content`). Refines A20-3, still open in v1.14.
- **location:** `deflate.hpp:224` `kMaxInflateOut`, checks at :229/:271/:286; gzip shrinking budget `stdlib_gzip.hpp:73-76`.
- **category:** test coverage
- **description:** No test crafts a small stream that inflates past the 256 MiB cap and asserts it throws. My probes confirm zlib, gzip, and multi-member gzip bombs all throw correctly, but a change to the cap check (or a missed budget on a new code path) would not be caught.
- **proposed-test:** a `.ki` that builds a small highly-repetitive stream (or reads a checked-in fixture) whose inflation exceeds the cap and asserts a "size limit" throw; separately assert the multi-member *aggregate* budget (A16-1 shrinking budget) fires. Keep the fixture tiny so CI stays fast.
- **confidence:** HIGH.

## Summary
- Total findings: 4
  - MEDIUM: 1 (A17-1 regex capture-DoS constant — cap too loose, no runtime budget)
  - LOW: 1 (A17-2 sub negative-count divergence)
  - Coverage-gap: 2 (A17-3 dynamic-Huffman/real-tool interop untested but correct; A17-4 bomb guard unasserted)
- No memory-safety, crash, OOB, or OOM defects found. Corruption handling, unicode, rejection
  paths, hash vectors, and Python-`re` conformance are all solid. The one live risk is the
  regex capture-group DoS constant (A17-1); the rest are correctness/coverage polish.
