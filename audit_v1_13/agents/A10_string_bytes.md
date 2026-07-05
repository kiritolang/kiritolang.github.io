# A10 — String (Unicode/code-point) + Bytes Audit

Area: String value type + all methods (code-point indexing/slicing/iteration, case, strip/split/join/replace, search, predicates, pad, partition, levenshtein, format mini-spec, f-string spec, str-vs-repr, encode/decode) and Bytes value type + all methods (immutable 0-255, indexing→Integer, slicing→Bytes, iteration→Integers, +/*, ordering, hashable, hex/fromhex, Bytes(x[,enc]), serialization).

Sources: `src/kirito/bytes.hpp`, String/Bytes impls in `src/kirito/runtime.hpp` (+ wherever else).
Tests: `tools/tests/unit/test_bytes.cpp`, `test_strbytes_deep.cpp`, `test_unicode.cpp`, `tools/tests/scripts/probe_unicode_conformance.ki`.

Status: COMPLETE.

Files read in full: `src/kirito/bytes.hpp`, `src/kirito/builtins.hpp` (utf8 helpers, StrVal, reprString, stringifyChild, case-mapping), `src/kirito/runtime.hpp` (StrVal::binary/getItem/slice/iterate/contains/getAttr method table, levenshtein, `applyFormatSpec`, chr/ord), `src/kirito/native.hpp` (sliceIndices), `src/kirito/bytecode_vm.hpp` (FormatValue). Tests: `test_bytes.cpp`, `test_strbytes_deep.cpp`, `test_unicode.cpp`, `test_fstring.cpp` (glanced), `probe_unicode_conformance.ki`, `r4_strings.ki`.

---

## Findings

### A10-1: `"İ".lower()` returns "ı" (U+0131) instead of "i" — case-folding bug in claimed Latin Extended-A range
- **Severity:** Medium (correctness bug; contradicts documented "covers all of Latin Extended-A U+0100..U+017F correctly").
- **Location:** `src/kirito/builtins.hpp` `utf8ToLowerCp`, the branch `if ((cp <= 0x0137) || ...) return (cp % 2 == 0) ? cp + 1 : cp;` (line ~73).
- **Category:** BUG / Unicode case folding.
- **Description:** The U+0100..U+0137 sub-block is handled by an even=upper / odd=lower alternation pairing (cp, cp+1). This holds for every pair EXCEPT the U+0130/U+0131 anomaly. U+0130 (İ, LATIN CAPITAL I WITH DOT ABOVE) and U+0131 (ı, LATIN SMALL DOTLESS I) are NOT a case pair in Unicode: simple-lowercase(U+0130) = U+0069 (`i`), and simple-uppercase(U+0131) = U+0049 (`I`). `utf8ToUpperCp` special-cases 0x0131→'I' and leaves 0x0130 unchanged (both correct), but `utf8ToLowerCp` has NO special case for 0x0130 — it falls into the alternation and returns cp+1 = 0x0131. So `"İ".lower()` yields "ı" (dotless i) rather than "i".
- **Failure scenario:** `"İ".lower() == "ı"` (bug) — expected `"i"`. Reachable via a source literal `"İ"` or `chr(0x130).lower()`.
- **Proposed test:** `assert chr(0x130).lower() == "i"` and `assert "İ".lower() == "i"` in `probe_unicode_conformance.ki`.
- **Proposed fix:** In `utf8ToLowerCp`, add `if (cp == 0x0130) return 'i';` before the range alternation (mirroring the existing 0x0131→'I' / 0x017F→'S' special cases in `utf8ToUpperCp`).
- **Confidence:** High (verified against Unicode UnicodeData simple mappings; the value-only asymmetry is clear in the code).

### A10-2: String `repr` (nested-in-container) does not escape 0x7f (DEL) or C1-range bytes, unlike Bytes repr and Python
- **Severity:** Low (cosmetic / repr-fidelity inconsistency).
- **Location:** `src/kirito/builtins.hpp` `reprString` (~line 262-278).
- **Category:** BUG (repr escaping correctness) / inconsistency.
- **Description:** `reprString` escapes only control bytes `< 0x20` (as `\xNN`) plus `\n \t \r \\` and the quote. The DEL character U+007F (byte 0x7f) and other non-printable code points ≥ 0x7f are emitted verbatim. Python's `repr` escapes `\x7f`. Meanwhile `BytesVal::str` (bytes.hpp line ~49) DOES escape 0x7f (`c >= 0x20 && c < 0x7f` verbatim, else `\xHH`). So `print([chr(0x7f)])` emits a raw DEL byte inside the quotes while `print(Bytes([0x7f]))` shows `b'\x7f'` — asymmetric. Non-ASCII code points (≥0x80) are also emitted as raw UTF-8, which is arguably a deliberate design choice (readable Unicode), but the 0x7f gap is an inconsistency, not a choice.
- **Failure scenario:** `String([chr(0x7f)])`-style container stringify shows an unprintable control char verbatim; round-trip/paste of the repr is ambiguous.
- **Proposed test:** assert `String([chr(0x7f)])` contains `\x7f` (once fixed), and compare against `Bytes([0x7f])` repr consistency.
- **Proposed fix:** In `reprString`, extend the control-escape to `uc < 0x20 || uc == 0x7f` (Python parity); leave ≥0x80 verbatim by design.
- **Confidence:** High (behaviour is directly readable; severity intentionally low).

### A10-3: `str.format` silently emits a lone `}` instead of raising (Python raises "Single '}' encountered")
- **Severity:** Low (leniency divergence).
- **Location:** `src/kirito/runtime.hpp` `StrVal::getAttr` `"format"` impl, the trailing `else { out += tmpl[i]; }` (~line 1368).
- **Category:** BUG (edge case) / divergence.
- **Description:** The template scanner handles `{{`, `}}`, and `{...}` fields. A single unescaped `}` that is not part of `}}` and not closing a field falls through to the raw copy branch and is emitted literally. Python's `str.format` raises `ValueError: Single '}' encountered in format string`. Kirito is lenient. (An unmatched `{` IS caught: "unmatched '{' in format string".) Low impact; may be intentional leniency but asymmetric with the `{` handling.
- **Failure scenario:** `"a } b".format()` returns `"a } b"` rather than erroring; a typo'd template passes silently.
- **Proposed test:** `throws(Function(): return "a } b".format())` — currently would FAIL (no throw).
- **Proposed fix:** In the final else, if `tmpl[i] == '}'` throw "single '}' in format string"; else copy. (Only if Python parity is desired.)
- **Confidence:** High on behaviour; Medium on whether it's considered a defect vs. intentional leniency.

### A10-4: `split()` with no separator splits only on ASCII whitespace (byte-wise), not Unicode whitespace
- **Severity:** Low (divergence from Python; documented coverage frontier).
- **Location:** `src/kirito/runtime.hpp` `StrVal::getAttr` `"split"` no-sep branch: `while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) ++i;` (~line 1300-1313).
- **Category:** BUG (Unicode) / divergence.
- **Description:** The no-separator whitespace split walks the string BYTE by byte using C-locale `std::isspace`. This only recognizes ASCII whitespace (space, \t, \n, \r, \f, \v). Unicode whitespace such as U+00A0 (NBSP, UTF-8 `C2 A0`), U+2028/U+2029, U+3000 are NOT split points (their bytes are all ≥0x80, isspace=false). Python's `str.split()` (no sep) splits on all Unicode whitespace. It is byte-safe (no UB — cast to unsigned char, continuation bytes 0x80-0xBF never register as ASCII whitespace, so no risk of splitting mid-code-point), so this is a fidelity gap only, not a corruption. Consistent with the documented ASCII-only frontier for predicates.
- **Failure scenario:** `"a b".split()` returns `["a b"]` (one field) rather than `["a", "b"]`.
- **Proposed test:** document/assert current behaviour: `"a" + chr(0x00A0) + "b"` split() yields one field (guards against silent regression), or extend to Unicode ws.
- **Proposed fix:** Decode code points and test a Unicode-whitespace set (would also align `isspace` predicate). Optional.
- **Confidence:** High.

### A10-5: `Bytes.apply` does not GC-root the per-byte Integer argument across the user `fn.call` (relies on small-int interning)
- **Severity:** Low (latent; not currently reachable because all args are 0..255 interned ints).
- **Location:** `src/kirito/bytes.hpp` `BytesVal::getAttr` `"apply"` (~line 282-295): `std::array<Handle, 1> args{vm.makeInt(c)};` then `deref(fn).call(vm, args)` with no `RootScope`.
- **Category:** GC-safety / robustness inconsistency (DRY vs `StrVal::apply`).
- **Description:** `StrVal::apply` (runtime.hpp ~1159) carefully wraps each per-char arg and the call result in a `RootScope` before/across the call, with a comment explaining GC exposure. `BytesVal::apply` does not root the `vm.makeInt(c)` argument nor the call result. It is safe today ONLY because byte values 0..255 are interned small integers that the collector will not sweep. If small-int interning were narrowed, or if this pattern is copied for a non-interned value, it becomes a dangling-handle bug. The `fn` handle itself is captured (rooted via bound method args), and `src` is a native copy (safe).
- **Failure scenario:** No live crash today; a refactor of int interning would expose a use-after-free of the argument during a mid-call GC.
- **Proposed test:** an `apply` that triggers heavy allocation in `fn` (forces GC) over a Bytes — should still be correct (guards the invariant).
- **Proposed fix:** Mirror `StrVal::apply`: `RootScope rs(vm); std::array<Handle,1> args{rs.add(vm.makeInt(c))};` and root the result before `asInt`.
- **Confidence:** Medium (safe in practice; flagged as latent robustness gap).

## Coverage gaps (behaviour present, tests missing or thin)

### A10-6: Bytes resource-guard limits untested
- **Severity:** Low (coverage).
- **Location:** `bytes.hpp` `makeBytes` `Bytes(n)` 256 MiB guard (`n > 256*1024*1024`), `BytesVal::binary` Mul `kMaxRepeat / data.size()` guard.
- **Description:** `test_bytes.cpp` tests `Bytes(3)`, `Bytes([256])`, `Bytes([-1])` but NOT: `Bytes(1<<40)` (huge-n throws "Bytes too large"), `Bytes(-5)` (negative-n throws "negative count"), `Bytes([1,2]) * 10**12` (repeat guard "repeated Bytes too large"), `Bytes([1,2,3]) * 0` and `* -1` (both → empty `b''`). These weak-spot guards are the crash/OOM defense and should have explicit tests.
- **Proposed test:** `CHECK_THROWS(Bytes(1099511627776))`, `CHECK_THROWS(Bytes([1])*1000000000000)`, `CHECK(Bytes([1,2])*0 == Bytes())`.
- **Confidence:** High.

### A10-7: `Bytes.apply` and `Bytes._getstate_`/`_setstate_` byte-level correctness untested; fromhex edge cases thin
- **Severity:** Low (coverage).
- **Description:** No test exercises `Bytes([...]).apply(fn)` at all (result-range guard `0..255`, non-Integer result throws, empty-fn throw). `fromhex` tests cover odd-length and non-hex but not: uppercase hex digits (`fromhex("ABCDEF")`), leading/trailing/internal whitespace runs, empty string (`fromhex("") == b''`), a single space then odd (`fromhex(" a")`). `hex()` on empty (`Bytes().hex() == ""`) untested. `_getstate_`/`_setstate_` are exercised only indirectly through serialize/dump round-trip, not the latin-1 lossless path across all 256 values (the 256-value latin-1 round-trip test in test_bytes uses decode/encode, not the getstate protocol).
- **Proposed test:** `Bytes([1,2,3]).apply(Function(b): return 255-b)`, `fromhex("ABCDEF")`, `Bytes().hex()`, `fromhex(" a")` throws.
- **Confidence:** High.

### A10-8: Case-folding boundary/anomaly code points untested
- **Severity:** Low (coverage) — but note A10-1 is a real bug this would have caught.
- **Description:** `probe_unicode_conformance.ki` covers Polish/Esperanto letters (clean alternating pairs) but NOT the anomalous / special-cased code points in Latin Extended-A: U+0130/U+0131 (İ/ı — the A10-1 bug), U+017F (ſ long-s → S in upper), U+0138 (ĸ kra, no case), U+0149 (ŉ, no simple mapping), U+0178 (Ÿ→ÿ). Also no round-trip assertion `cp.upper().lower()` sweep across U+0100..U+017F.
- **Proposed test:** add explicit vectors for İ/ı, ſ→S, Ÿ↔ÿ; a loop asserting idempotence `s.upper().upper()==s.upper()` over the range.
- **Confidence:** High.

### A10-9: Malformed-UTF-8 robustness of String METHODS (not just decode) untested; utf8DecodeAt returns raw lead byte on truncation
- **Severity:** Low (defense-in-depth; not currently reachable via normal String construction).
- **Location:** `builtins.hpp` `utf8DecodeAt` returns the raw lead byte `c` when a multibyte sequence is truncated (`i + k >= s.size()`). `utf8Starts` treats an unknown/`0xF8+` lead as width 1.
- **Description:** Kirito Strings are guaranteed valid UTF-8 (constructed from validated `decode`, `chr` with surrogate/range guards, and lexed literals), so `upper`/`strip`/`index`/`slice` never see malformed UTF-8 in practice. There is no direct test that FORCES a malformed-UTF-8 String through the code-point methods (there is no public path to build one), so this is correctly a non-issue — but it is an undocumented invariant relied upon by `mapCase`, `classify`, `window`, `byteToCp`. If any future native path constructs a `StrVal` from unvalidated bytes, `utf8DecodeAt`/`utf8Starts` degrade silently rather than throwing.
- **Proposed test:** none feasible from Kirito (no malformed-String constructor); a C++ unit test could assert `StrVal` construction is the sole trust boundary. Documentation note recommended.
- **Confidence:** Medium (invariant holds today).

## DRY / observations (no defect)

### A10-10: String vs Bytes method-structure duplication and shared helpers — assessment
- `bytesutil::encode/decode/normEnc/toHex/fromHex/validUtf8` are well-centralized in `bytes.hpp` and reused by `String.encode` (runtime.hpp calls `bytesutil::encode`) and the codec modules via `argStringOrBytes`/`makeStringOrBytes` — good DRY.
- `StrVal::apply` and `BytesVal::apply` duplicate the map-loop shape (see A10-5 for the GC asymmetry) but over different element types; acceptable.
- `applyFormatSpec` is the single mini-spec implementation shared by the `format` builtin and f-string `FormatValue` (bytecode_vm.hpp) — good, no duplication.
- `str.format` (`{}`/`{n}` only, no `:spec`/named fields) is DIFFERENT from the mini-spec — this is intentional and explicitly tested (`r4_strings.ki` "format-spec-field-throws", "format-named-field-throws"). NOT a defect; documented divergence from Python.
- Slice/index helpers (`sliceIndices`, `sequenceIndex`, `singleKey`, `cpIndexToByte`) are shared and robust (zero-step throws, count-driven loop avoids INT64 step overflow, code-point mapping for String).
- `-Wconversion`: the byte/code-point conversions use explicit `static_cast<...>` throughout (`static_cast<char>(static_cast<unsigned char>(...))`); no obvious narrowing risk spotted.

## Positives confirmed (no action)
- Bytes `*` repeat, String `*` repeat, `ljust/rjust/center` width, `zfill`, `replace` (empty-pattern interleave), `join` all guard against `kMaxRepeat` (256 MiB) before allocating — clean catchable errors, no OOM/overflow.
- `Bytes(n)` guards negative and >256 MiB. `sliceIndices` guards zero step and INT64 step overflow.
- `encode`/`decode` validate UTF-8 (overlong/surrogate/truncated/stray-continuation all throw — tested in `test_strbytes_deep.cpp`), reject unknown encodings and out-of-cap code points/bytes. latin-1 256-value round-trip tested.
- `chr` rejects surrogates and out-of-range; `ord` requires exactly one code point — so Strings stay valid UTF-8 (upholds the A10-9 invariant).
- Code-point vs byte discipline is correct: String indexes/slices/iterates/lengths by code point (ASCII fast-path + cached `codePointStarts`), Bytes by byte; String ordering is bytewise which equals code-point order for valid UTF-8.
- `applyFormatSpec` width/precision counted in CODE POINTS (multibyte no under-pad), high-precision snprintf sized dynamically (no truncation), string-precision truncates by code point.
