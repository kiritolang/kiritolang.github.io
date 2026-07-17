# A07 Strings/Bytes

Status: IN PROGRESS — reading source (bytes.hpp, common.hpp done), now scanning runtime.hpp String methods + format.

Scope: src/kirito/bytes.hpp, String/Bytes methods+format+str/repr in src/kirito/runtime.hpp, common.hpp.

## Notes from bytes.hpp / common.hpp read

- `bytesutil::validUtf8` looks correct: checks lead bytes, continuation bytes, overlong, surrogate range, out-of-range >0x10FFFF. Good.
- `fromHex` skips whitespace but only checks `std::isspace` — note it does NOT reject a hex string of odd length after skipping whitespace correctly (checks `i+1>=s.size()` each pair) — looks fine actually. But whitespace skip is only 1 char at a time and only checked before start of a pair, not between the two nibbles of a pair (e.g. "a b" -> at i pointing to 'a', i+1 is ' ' -> nib(' ') = -1 -> throws "non-hex digit" rather than skipping — inconsistent but arguably fine/edge case.
- `makeBytes` for Integer count uses cap 256MiB — matches kMaxRepeat scale, good guard against `Bytes(-1)` (throws "negative count") and huge n.
- Bytes `*` repetition guarded via kMaxRepeat.
- `Bytes.decode` default enc "utf-8"; encode/decode symmetric via bytesutil.
- `contains` for Integer out of 0-255 range returns `false` rather than throwing — mild, will check String equivalent behavior.

Continuing to runtime.hpp next.

## Deep read: runtime.hpp String methods (getAttr ~1147-1585), applyFormatSpec (~2694-2836), builtins.hpp (reprString/stringifyChild/utf8* helpers)

Traced code-point-vs-byte handling end to end:
- `window()` helper (runtime.hpp ~1170) converts optional `[start[, end]]` via `cpIndexToByte` for
  find/index/rfind/rindex/count/startswith/endswith — all CODE-POINT correct (verified against
  `cpIndexToByte`'s clamp-negative/clamp-past-end logic). Adversarial case `"café".find("f", 2)`
  traced by hand: codepoints c(0) a(1) f(2) é(3) — cp 2 correctly maps to the byte offset of 'f'.
  No byte/code-point confusion found in any of these methods.
- `split()`, `join()`, `replace()`, `partition`/`rpartition`, `strip`/`lstrip`/`rstrip`,
  `ljust`/`rjust`/`center`, `zfill`, `levenshtein`, `apply` — all operate on `utf8Starts()`-derived
  boundaries or code-point counts (`utf8Length`), never raw byte length, where the semantics call for
  code points. Default (no-arg) `split()`'s whitespace scan is byte-wise but safe: ASCII whitespace
  bytes (<0x80) can never appear as a UTF-8 continuation/lead byte, so a multi-byte character can never
  be split mid-sequence.
- `bytesutil::validUtf8` (bytes.hpp) correctly rejects overlong encodings, surrogate-range code points,
  out-of-range (>U+10FFFF), and truncated sequences — checked by hand against the RFC 3629 lead-byte
  table. `chr()` also rejects surrogates (0xD800-0xDFFF) and out-of-range values, so a String can never
  contain an unpaired surrogate to begin with.
- `applyFormatSpec` (the mini-spec: fill/align/sign/`#`/0/width/`,`/precision/type) matches the spec in
  CLAUDE.md; `#` alt-form prefix, `,` grouping (base-10 only, rejected for b/o/x), precision rejected for
  integer types, sign/`#`/`=`/`,` all rejected for string-typed specs — every rejection path is
  exercised by an existing test (see coverage below).
- `String.format()` (the plain `{}`/`{N}` substitution, runtime.hpp ~1393) is a DELIBERATELY simpler
  implementation than the f-string mini-spec path: it does not parse a `:spec` at all (a colon in the
  field body isn't a digit, so it throws "format field must be an index") and does not support named
  fields (`{name}`). This is NOT a bug — it is explicitly pinned by
  `tools/tests/scripts/r9_types_err.ki:111-113` ("str.format named placeholder throws", "str.format with
  format-spec throws", "str.format indexed+spec throws") and `labx_string_type.ki:278-281`. Consistent
  with the existing false-positives table's entry on `.format()`'s auto/indexed mixing rule — this is
  the same family of intentional restriction. Recorded here so a future round doesn't re-flag it.

## Adversarial cases traced (all confirmed correct / already guarded)

- `"café".find("f", 2)` → correct byte offset via `cpIndexToByte` (see above).
- `"".split("")` and `"abc".split("")` → both throw "empty separator" (matches Python's ValueError for
  an empty separator); tested in `labx_string_type.ki:118`.
- `"a,,b".split(",")` → `["a", "", "b"]`, tested (`labx_string_type.ki:113`).
- `"x".zfill(-1)` → not explicitly tested (see coverage gap below), but traced by hand: `width=-1`;
  the huge-width guard only rejects `width>0` cases (`static_cast<uint64_t>(width<0?0:width)`), so a
  negative width slips past the guard, but then `pad = width - len(s)` is negative and the method
  returns `s` unchanged — safe, no crash, consistent with `center(-3)` (which IS tested).
- `b"\xff\xfe".decode("utf-8")` → throws "invalid UTF-8 byte sequence" (0xFF is never a valid lead
  byte); tested (`labx_bytes.ki:90-92`, including surrogate-encoded and overlong variants).
- `"ab".encode("ascii")` → succeeds (`labx_string_type.ki:256`); `"café".encode("ascii")` → throws
  "codec can't encode" (tested, `labx_string_type.ki:257`; exact `U+00E9`-style message tested in
  `labx_bytes.ki:88-89` for latin-1).
- `Bytes([256])` → throws "element out of range (0..255)" (tested, `labx_bytes.ki:33`).
- `"{:#x}".format(255)` → N/A: `.format()` has no spec support (throws, by design — see above); the
  equivalent `format(255, "#x")` / f-string `f"{255:#x}"` DOES support it and is tested
  (`test_fstring.cpp:28`, `labx_string_type.ki:290`) — confirmed correct ("0xff").
- `"{:.2f}".format("hi")` → N/A for the same reason; `format("hi", ".2f")` correctly throws "format
  type 'f' needs a number, not 'String'" (traced in applyFormatSpec's numeric-type guard).
- `s.center(10**9)` / huge ljust/rjust/zfill widths → guarded by `kMaxRepeat` (256 MiB), consistent
  with the documented resource-guard policy; the byte-vs-code-point double accounting for a multibyte
  fill char is explicitly handled (`kMaxRepeat / fill.size()` after the code-point width check) so a
  4-byte fill char can't sneak past the cap via unit confusion.
- `fromhex("zz")` / `fromhex("abc")` (odd length) → exact messages "non-hex digit" / "odd-length hex
  string", tested (`labx_bytes.ki:106-107`), plus the subtler case of whitespace splitting a byte pair
  (`fromhex("4 8")` → "non-hex digit", tested at `labx_bytes.ki:109`) which I verified is intentional:
  whitespace is skipped only BETWEEN complete byte pairs, not between the two nibbles of one pair.

## DRY / structure notes (no action needed)

- `cpIndexToByte`/`byteToCp`/`window` are shared closures reused by every code-point-window method
  (find/index/rfind/rindex/count/startswith/endswith) — good, no duplicated logic.
- `bytesutil::encode`/`decode`/`normEnc` are the single source of truth for String.encode /
  Bytes.decode / the `_getstate_`/`_setstate_` serialization round-trip (which reuses "latin-1") — no
  duplication found.
- `kMaxRepeat` (common.hpp) is consistently the single cap for String `*`, Bytes `*`, ljust/rjust/
  center/zfill widths, replace/join result growth, and format width/precision — verified each call site
  divides by the element size (not a flat byte count) where relevant, avoiding a unit-confusion
  under-guard.

## Coverage gaps (minor, non-blocking — existing suite is unusually thorough for this subsystem)

1. `"x".zfill(-1)` (or any String).zfill with a negative width — not directly tested (ljust/rjust/
   center all have a negative-width test; zfill doesn't). Traced safe by hand (see above) but no
   regression pin exists.
2. No test found for `"x".rjust(-1)` / `"x".ljust(-1)` explicitly (center(-3) is tested; rjust/ljust
   only test width==0, not negative).
3. `format()`/`.format()` precision very close to (but under) the `kMaxRepeat` cap, e.g.
   `format(1.5, ".100000000f")` (~100M digits, under the 256 MiB guard) — only the well-over-cap case
   (`.2345678901f`, ~2.3B) is tested; the "large but technically allowed" middle ground isn't exercised
   (would allocate/format a ~100MB string — slow but not wrong; not flagged as a bug, just an untested
   perf edge).
4. No explicit test combining `,` grouping with a negative Integer in `format` (e.g. `format(-1234567,
   ",d")`) to confirm the sign lands before the grouped digits, though the code path (`signStr` prefixed
   separately from `groupThousands(digits)`) looks correct by inspection.

No bugs found in this subsystem beyond what prior rounds already fixed. The Bytes/String surface has
unusually deep adversarial coverage already (`labx_bytes.ki`, `labx_string_type.ki`, `r9_types_err.ki`),
covering nearly every case in my adversarial checklist before I could construct a novel repro.

## Summary

Scanned: src/kirito/bytes.hpp, src/kirito/common.hpp, the String/Bytes method surface + applyFormatSpec
+ str/repr helpers in src/kirito/runtime.hpp and src/kirito/builtins.hpp, plus the f-string spec-split
path in bytecode_vm.hpp (FormatValue op) for context.

Findings: 0 confirmed bugs. All adversarial cases from the assignment (code-point-vs-byte find offsets,
empty-separator split, invalid/surrogate/overlong UTF-8 decode, ascii encode range check, Bytes(256),
format `#`/precision-on-string, fromhex validation) were traced and found correctly handled, most with
an existing pinned regression test. The one interesting-looking gap (`.format()` not supporting named
fields / format-specs, unlike f-strings) is confirmed intentional and already pinned in
`r9_types_err.ki`/`labx_string_type.ki` — recorded here explicitly so it isn't re-flagged as a bug in a
future round. Filed 4 minor, non-blocking coverage-gap notes (zfill/rjust/ljust negative width,
near-cap format precision, negative-number `,` grouping) for a future test-writing pass; none rise to
the level of a fix-worthy finding.

Status: DONE.

