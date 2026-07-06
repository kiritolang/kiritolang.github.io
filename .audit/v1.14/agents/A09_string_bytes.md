# A09 — String + Bytes (v1.14 audit)

**Status: IN PROGRESS**

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

