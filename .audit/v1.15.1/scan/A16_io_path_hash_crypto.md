# A16 — io + path + hash + crypto + zlib/gzip (v1.15.1)

Scope: `stdlib_io.hpp`, `stdlib_path.hpp`, `stdlib_hash.hpp` + `hashing.hpp`,
`stdlib_crypto.hpp`, `stdlib_zlib.hpp` / `stdlib_gzip.hpp` + `deflate.hpp`.

Probing binary: `./build-debug/ki` (TLS ON). All probes under a temp dir.

Pinned false positives honoured (not re-litigated):
- `io.BytesIO.seek(-n)` CLAMPS to 0 while `File.seek(-n)` THROWS — by design.
- `base64.decode` REJECTS embedded whitespace/newlines — by design.
- `io.write`/`io.print` stringify a Bytes rather than writing raw bytes — intentional.

v1.15 already fixed here (not re-fixed): A13-1 (crypto sign/verify key-FAMILY check),
A13-2 (`pbkdf2Raw` iterations>=1 guard).

---

## Findings

### A16-1: `hash.pbkdf2` SILENTLY truncates `iterations` to 32 bits — a 2^32+1 work factor runs ONE iteration  [severity: HIGH] [confidence: confirmed]
- Location: `src/kirito/stdlib_hash.hpp:69,77-78` (`int64_t iters = args[2].asInt(...)` … `static_cast<uint32_t>(iters)`)
- What: the binding validates `iters` as an **int64** (`if (iters < 1) throw`), then narrows it to
  `uint32_t` when calling `hashing::pbkdf2Raw`. Any `iterations` ≥ 2^32 wraps modulo 2^32, so a caller
  who asks for a huge work factor gets a *tiny* one — with **no error**. `2^32+1` → 1 iteration,
  i.e. PBKDF2 degrades to a single HMAC, exactly the weak-KDF outcome the A13-2 `iterations >= 1`
  guard in `pbkdf2Raw` was added to prevent. A13-2 only closed the `0` case (which throws); the
  truncation *past* the wrap is wide open. The `< 1` check gives false assurance: it is applied to a
  value that is not the one actually used.
- Repro (`/tmp/a16/p1.ki`, real output):
```
$ ./build-debug/ki /tmp/a16/p1.ki
iters=1      : 120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b
iters=2^32+1 : 120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b
EQUAL(1 vs 2^32+1)? True
EQUAL(2 vs 2^32+2)? True
iters=2^32 threw: pbkdf2: iterations must be >= 1
```
  (`120fb6cf…` is the RFC 6070-style known answer for PBKDF2-HMAC-SHA256("password","salt", **c=1**),
  so 2^32+1 provably performed a single iteration. Note the tell-tale cliff: 2^32 → 0 → throws, but
  2^32+1 → 1 → silently succeeds.)
- Impact: a security control silently voided. Anyone hardening a password hash by raising the
  iteration count (a config value, a value scaled from a benchmark, or an attacker-influenced
  parameter stored beside the hash) can land ≥ 2^32 and get a **single-HMAC** "PBKDF2" that still
  returns a plausible-looking derived key and verifies consistently against itself — undetectable
  without a known-answer comparison. Silent weakening is worse than a throw.
- Proposed fix: range-check against the type actually used, before narrowing:
  `if (iters < 1 || iters > std::numeric_limits<uint32_t>::max()) throw KiritoError("pbkdf2: iterations must be in [1, 4294967295]");`
  Breaks no documented contract (no doc promises > 2^32-1 iterations; the current behaviour is
  strictly a silent mis-computation). Alternatively widen `pbkdf2Raw` to `uint64_t`, but a cap is
  the better call — 2^32 iterations already takes hours, so the ceiling is a usability guard too.
- Proposed test: `tools/tests/unit/test_hash.cpp` (or `test_compress_hash_deep.cpp`) —
  `pbkdf2("password","salt",4294967297)` must THROW, and must **not** equal
  `pbkdf2("password","salt",1)`. Fails on the unfixed build (currently returns, and is equal).
  Mirror in `tools/tests/scripts/deep_compress.ki`-style `.ki` coverage.

### A16-2: `File.writelines(<write-only stream>)` leaks a raw `bad optional access`  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/stdlib_io.hpp:216-218` (`auto items = ...iterate(vm); ... for (Handle h : items.value())`)
- What: `writelines` calls `.value()` on the `std::optional` returned by `Object::iterate` without
  checking it. Most non-iterables (Integer/None/Float/Bool) *throw* inside `iterate`, so the happy
  path hides this — but a type that legitimately returns **`std::nullopt`** reaches `.value()` and
  raises `std::bad_optional_access`. `StdStream::iterate` does exactly that for a write stream
  (`stdlib_io.hpp:463`: `if (dir != Dir::In) return std::nullopt;`). The message has no line/col and
  no clue what went wrong, breaking the "clear diagnostics / errors are part of the language" rule.
  Compare the correct handling one call away: `List(io.stdout)` → `List() argument must be iterable`.
- Repro (`/tmp/a16/io3.ki`, real output):
```
$ ./build-debug/ki /tmp/a16/io3.ki
writelines(io.stdout) THREW: bad optional access
--- survived stdout ---
writelines(io.stderr) THREW: bad optional access
--- survived stderr ---
List(io.stdout) THREW: List() argument must be iterable
```
- Impact: low — it is catchable (the bare-`catch` std::exception boundary converts it to a String),
  so no crash; the cost is a baffling diagnostic for a plausible typo (`f.writelines(io.stdout)`
  when `io.stdout` was meant as the *destination*). Any other native type returning `nullopt` from
  `iterate` hits the same path.
- Proposed fix: check before use, matching the surrounding style —
  `if (!items) throw KiritoError("writelines: argument must be iterable");`
  No documented contract at risk.
- Proposed test: `tools/tests/unit/test_io.cpp` — `f.writelines(io.stdout)` must throw an error whose
  text contains `iterable` (asserting the *message*, since the current failure is also an exception).
