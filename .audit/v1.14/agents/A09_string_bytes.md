# A09 — String + Bytes (v1.14 audit)

**Status: COMPLETE**

## Summary
- Findings: 4 (0 high, 1 medium, 3 low). 1 documented non-bug, 1 coverage gap.
  - A09-1 (MEDIUM): `format(x)` empty-spec is lossy for floats ('g' precision-6) and disagrees
    with `f"{x}"` / `"{}".format(x)` / `String(x)`, contradicting its own comment/test intent.
  - A09-2 (LOW): zero-pad + `,` grouping doesn't group the pad zeros (Python-mismatch, cosmetic).
  - A09-3 (LOW): `isalpha`/`isalnum` treat every code point >= 0x80 as a letter (emoji/symbols).
  - A09-4 (LOW): inf/nan zero-padding uses '0' not spaces (cosmetic).
- Encode/decode, code-point indexing/slicing/astral, f-strings, Bytes, str-vs-repr, and the full
  format mini-spec are otherwise robust and correctly guarded.



Subsystem: `src/kirito/builtins.hpp` (StrVal + utf8 helpers), `src/kirito/runtime.hpp` (String
methods + format mini-spec), `src/kirito/bytes.hpp` (Bytes), encode/decode.

Runner: `/home/user/kiritolang.github.io/build-debug/ki`

## Map
- StrVal class: `builtins.hpp:164`. UTF-8 lenient decode (`utf8Starts`, `utf8DecodeAt`) — invalid
  UTF-8 never throws at construction; strings are byte-transparent.
- String methods: `runtime.hpp` (`getAttr` from ~1196).
- Format mini-spec: `runtime.hpp`.
- Bytes: `bytes.hpp`.

## Findings

### A09-1: `format(x)` with empty spec is lossy for floats and inconsistent with `f"{x}"` / `String()`
- severity: MEDIUM
- location: `runtime.hpp:3284-3295` (format builtin) + `runtime.hpp:2698-2705` + `applyFormatSpec` empty-type rule; contrast `bytecode_vm.hpp:277`.
- category: correctness / consistency / data-loss surprise
- description: The `format` builtin with no spec (or `format(x, "")`) routes an empty spec
  into `applyFormatSpec`, which for a Float defaults the presentation type to `'g'` with the
  default precision **6**. So `format(2.0)` → `"2"` (drops the `.0`), `format(1000000.0)` →
  `"1e+06"`, and `format(1234567.0)` → `"1.23457e+06"` — which **does NOT round-trip** (loses
  digits). Meanwhile the f-string empty-spec path (`bytecode_vm.hpp:277`,
  `spec.empty() ? stringify : applyFormatSpec`) and `String(x)` both use `stringify` →
  `"2.0"`, `"1000000.0"`, `"1234567.0"` (exact). The code comment at runtime.hpp:3286 ("No spec
  == String()") and at 2698-2701 ("matching `String()`") are **both wrong**: format() does NOT
  match String() for floats.
- failure-scenario: `format(user_float)` in a report silently truncates a 7+ significant-digit
  float to 6 sig figs in scientific notation — a data-loss bug the user cannot see coming.
- confirmed: `format(2.0)`→`2`, `format(1234567.0)`→`1.23457e+06`, but `f"{x}"`/`String(x)`→`1234567.0`.
- proposed-test: `.ki` asserting `format(2.0) == "2.0"` and `format(1234567.0) == "1234567.0"`
  (matching String / f-string), plus a doc-example round-trip check.
- proposed-fix: in the format builtin (or applyFormatSpec) short-circuit `if (spec.empty()) return
  vm.stringify(value);` exactly as the f-string path does — then all three (`format`, `f"{}"`,
  `String`) agree and no precision is lost.
- confidence: HIGH (behavior confirmed, comment demonstrably inaccurate)

### A09-2: zero-pad + `,` grouping does not group the pad zeros (Python-mismatch, cosmetic)
- severity: LOW
- location: `applyFormatSpec` runtime.hpp:2741 (grouping applied to significant digits only) + 2780-2788 (padding).
- category: consistency (deviation from the Python-like model the mini-spec advertises)
- description: `format(1234, "012,d")` → `"00000001,234"`; Python gives `"0,001,234"` (the pad
  zeros participate in grouping). Kirito groups only the significant digits then left-pads with
  ungrouped zeros. Width is still honored; only the grouped appearance differs.
- failure-scenario: cosmetic; a user copying a Python format string gets differently-grouped output.
- proposed-test: `format(1234, "08,d")` expected value documented either way.
- proposed-fix: when `zero && comma`, group after zero-padding to width (as CPython does), or
  document the divergence. Low priority.
- confidence: HIGH (confirmed)

### A09-3: `isalpha`/`isalnum` classify EVERY non-ASCII code point as a letter (emoji, symbols, superscripts)
- severity: LOW
- location: `runtime.hpp:1437-1440` (`isalpha`/`isalnum` predicates: `... || c >= 0x80`).
- category: correctness (documented simplification, but produces clearly wrong results)
- description: The predicates treat any code point `>= 0x80` as a letter, so `"😀".isalpha()` →
  `True`, `"²".isalpha()` → `True` (superscript two), and any emoji/CJK-punctuation/symbol string
  reports as alphabetic. `isdigit` is ASCII-only so `"²".isdigit()`/`"①".isdigit()` → `False`
  (also arguably wrong, but conservative). This is the deliberate "non-ASCII letters count"
  shortcut, but it misclassifies the entire non-letter side of the BMP + astral planes.
- failure-scenario: input validation using `.isalpha()` accepts emoji/symbols as letters.
- proposed-test: `.ki` documenting `"😀".isalpha()` result (lock current behavior or the fix).
- proposed-fix: gate on a Unicode letter-category table for at least the covered ranges (the same
  ranges `upper`/`lower` handle), or document the coarse behavior explicitly in 09-types.md. Low
  priority — matches the stated "future full-Unicode" limitation.
- confidence: HIGH (confirmed)

### A09-4: infinity/NaN zero-padding pads with '0' instead of spaces (cosmetic Python-mismatch)
- severity: LOW (informational)
- location: `applyFormatSpec` runtime.hpp:2780-2788, no special-casing of non-finite bodies.
- category: consistency
- description: `format(math.inf, "010.2f")` → `"0000000inf"`. CPython ignores the `0`/zero-fill
  for inf/nan and pads with spaces (`"       inf"`). Harmless but visually odd.
- proposed-fix: when the numeric body is `inf`/`nan`, force `fill=' '` and align (or skip zero-pad).
- confidence: HIGH (confirmed)

## Non-findings (verified correct, worth recording)
- Encoding boundaries all robust: utf-8 decode rejects invalid/overlong/truncated/surrogate
  sequences with a clear catchable error; latin-1 round-trips 0..255 losslessly; ascii>127 throws
  on both encode and decode; empty String/Bytes round-trip; BOM is kept as U+FEFF (correct — not
  utf-8-sig). `chr` rejects surrogates and >U+10FFFF; astral (>U+FFFF) index/slice/len/ord all
  code-point-exact.
- `format` mini-spec: fill+align (`<>^=`), sign (`+ - space`), `#` alternate (0b/0o/0x/0X), `0`-pad,
  width (guarded), `,` grouping (rejected on non-decimal / string), precision (rejected on integer),
  type d/f/e/g/b/o/x/X/s/% — all correct; width/precision bounded against DoS.
- f-strings: nested single-quoted dict keys `f"{d['k']}"`, braces inside inner string literals
  `f"{'}'}"`, `:spec`, whitespace in braces, `{{`/`}}` escaping, and a throwing inner expr all work.
- String methods with optional args (find/rfind/index/count/startswith/endswith start/end;
  split sep/maxsplit incl. empty-sep throw and maxsplit=0/-1; replace empty-old/count=0/-1;
  strip(chars); partition/rpartition no-match; ljust/rjust/center/zfill width guard + multibyte)
  all match Python semantics.
- Bytes: `b[i]`→Integer, slice→Bytes (incl. `[::-1]`/step), iteration→Integers, `Bytes(n)`=n zeros,
  `Bytes(list)` out-of-range/negative throw, `fromhex` odd-length/non-hex throw + whitespace-skip,
  unsigned lexicographic ordering, hashable, `+`/`*` (repeat guarded), typed concat errors,
  `_getstate_`/`_setstate_` via latin-1. `String == Bytes` is `False` both ways.
- str-vs-repr: `[""]`→`['']`, quote-switching, both-quotes escaping, `\n`/`\t`/`\r`/control `\xHH`
  all correct.
- `A09-note`: The String `.format()` method rejecting `:format-spec` (`"{:.2f}".format(x)` throws
  "format field must be an index") is **documented by design** (09-types.md:145 directs users to
  f-strings / `format()` builtin) — NOT a bug.

## Coverage gaps (C++ vs .ki)
- The invariant asserted by `tools/tests/scripts/r4_numbers.ki:404`
  (`format(3.5) == "3.5"`, comment "format with no spec == String()") and
  `tools/tests/unit/test_stdlib_extra.cpp:72` (`format(42) == "42"`) only exercise values where the
  `'g'` default coincidentally matches `String()`. **No test covers `format(2.0)` (would expose
  `"2"` ≠ `"2.0"`) or `format(1234567.0)` (would expose the lossy `"1.23457e+06"`)** — this is the
  gap that let A09-1 persist. Recommend a test asserting `format(x)` agrees with `f"{x}"` /
  `"{}".format(x)` / `String(x)` for a range of floats.
- Bytes/encode/decode/f-string/format are otherwise densely covered in C++
  (`test_bytes.cpp`, `test_strbytes_deep.cpp`, `test_fstring.cpp`, `test_string_ops.cpp`,
  `test_string_literals.cpp`) and `.ki` (`probe_unicode_conformance.ki`, `probe_strings.ki`,
  `probe_bytes_fuzz.ki`, `deep_text.ki`, `r4_strings.ki`).

