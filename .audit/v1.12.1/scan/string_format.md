# v1.12.1 (audit loop) — String type, f-strings, format mini-spec

Subsystem: String method table (`src/kirito/runtime.hpp` StrVal::getAttr ~L1126-1561),
format mini-spec (`applyFormatSpec` ~L2660-2802), `format` builtin (~L3297), f-string
(`Op::FormatValue`/`BuildString` in bytecode_vm.hpp ~L273), reprString (builtins.hpp L262),
levenshtein (runtime.hpp L1106), string indexing/slicing (runtime.hpp L1024-1095).

Probe binary: ./build-debug/ki

## LOG
- Read StrVal::getAttr method table, getItem/slice/iterate, binary, applyFormatSpec, format builtin,
  reprString, levenshtein. Starting probes.

## Scope covered (all PROBED with ./build-debug/ki, confirmed behavior)
- Format mini-spec: fill/align(< > ^ =)/sign(+ - space)/#/0/width/,/.precision/type(d b o x X f e g % s),
  empty type per-kind, INT64_MIN, nan/inf, negative, sign-aware = padding, explicit-align+0 flag,
  multibyte fill (cleanly rejected — continuation bytes never collide with align chars), width/precision
  caps, all documented error cases. SOLID — behaves correctly incl. Python-parity on the tricky combos.
- String methods: getitem/slice (multibyte via chr(0x1F600), negative, oob throws, step, ::0 error,
  ::-1), find/rfind/index/rindex/count/startswith/endswith with code-point [start[,end]] incl. start>end
  / negative / empty sub, split (default ws / sep / maxsplit 0 / neg / empty-sep error / trailing),
  strip(chars)/lstrip/rstrip (code-point aware), replace (count 0 / neg / empty old / empty-old+count),
  ljust/rjust/center (multi-char fill error, width<len), zfill (sign, neg width), partition/rpartition
  (missing sep), removeprefix/removesuffix (empty), is{digit,alpha,alnum,space,lower,upper} (empty/
  unicode/mixed/uncased), upper/lower (Latin-1 é→É, Latin-Ext-A Ą→ą, emoji unchanged), apply,
  encode (utf-8/latin-1 lossy error), levenshtein (String / List / bad arg / unicode), * (0/neg/huge
  guard) and +. ALL CORRECT.
- f-strings: basic/spec/float-spec/whitespace-in-braces/{{ }}/nested quotes f"{d['k']}"/expr/two-exprs/
  brace-in-strlit/nested f-string/empty. Nested spec f"{a:{b}}" cleanly rejected (documented limitation).
- str.format(): {}/{N}/{0} reuse; no format-spec support is DOCUMENTED (09-types.md:150) => by-design.

## FINDINGS

### F1 [LOW] repr of a String does not escape DEL (U+007F) — raw control byte embedded in repr
- where: src/kirito/builtins.hpp:262 (reprString), escapes only `uc < 0x20`
- repro: `io.print(["a" + chr(0x7f) + "b"])`  -> bytes `5b 27 7f 27 5d` i.e. `['a<DEL>b']`
- actual: the raw 0x7F DEL byte is emitted verbatim inside the quoted repr
- expected: repr should be unambiguously printable — Python shows `'\x7f'`. reprString's contract is
  "reads back unambiguously when shown inside a container"; a raw DEL breaks that (invisible/mangling
  in a terminal). (Bytes 0x80+ are correctly left raw — they are UTF-8 multibyte, not lone controls.)
- fix idea: also escape 0x7F (and optionally the 0x80–0x9F C1 range if a lone-control string were ever
  representable — but those aren't reachable as valid UTF-8) as `\xHH`.

### F2 [LOW] str.format(): a lone unmatched `}` is silently passed through instead of erroring
- where: src/kirito/runtime.hpp:1395-1400 (format method) — only `}}` is handled; a bare `}` falls to
  the final else and is appended raw
- repro: `"a}b".format()` => `"a}b"` ;  `"x } y".format()` => `"x } y"`
- actual: a single `}` is emitted literally
- expected: Python raises "Single '}' encountered in format string". Contrast: a lone `{` DOES throw
  ("unmatched '{'"), so the two braces are treated asymmetrically. Minor silent-error inconsistency.
- fix idea: throw on a `}` not immediately followed by another `}` (mirror the `{` handling).

### F3 [LOW] `,` grouping combined with `0` zero-padding does not regroup the pad zeros
- where: src/kirito/runtime.hpp:2800 — `=` align path is `signStr + string(pad,'0') + body`; body is
  already comma-grouped, so the leading zero run is inserted ungrouped
- repro: `format(1234567, "015,d")` => `"0000001,234,567"`
- actual: 15-wide but the leading zeros are not comma-separated
- expected: Python groups the padding too: `"000,001,234,567"`. Cosmetic deviation on an edge combo
  (zero-pad + thousands). Not a crash.
- fix idea: when `zero && comma && align=='='`, re-run groupThousands over `pad-zeros + body` (with the
  digit count aligned to 3s), or compute width via the grouped string.

### F4 [LOW] a precision dot with no digits (`.` / `.f`) is accepted as precision 0 rather than an error
- where: src/kirito/runtime.hpp:2684-2693 — the `.` branch loops 0 digits, leaving precision=0
- repro: `format(5.0, ".")` => `"5"` ;  `format(5.0, ".f")` => `"5"`
- actual: treated as precision 0 (g/f with 0 sig digits/decimals)
- expected: Python raises "Format specifier missing precision". Minor; a lone dot is unusual input.
- fix idea: require >=1 digit after `.` else throw invalid-spec.

### F5 [SUSPECT/by-design] isalpha/isalnum classify EVERY code point >= 0x80 as a letter
- where: src/kirito/runtime.hpp:1438-1441 — `c >= 0x80` counts as alpha/alnum
- repro: `(chr(0x263A)).isalpha()` (WHITE SMILING FACE) => True ; emoji `.isalpha()` => True
- actual: symbols, punctuation, emoji, etc. all report as letters
- expected: a smiley/emoji is not a letter; Python uses the Unicode category. The doc comment
  ("non-ASCII letters count") shows this is a deliberate simplification (no Unicode category table),
  so likely BY-DESIGN, but it is a real over-broad classification worth noting.
- fix idea: (if ever tightened) consult a code-point category table; currently accepted tradeoff.

## SUMMARY
Subsystem is robust. No HIGH/MED defects found. Format mini-spec matches Python on all the hard
combos (sign-aware `=`, `#`+zero-pad, INT64_MIN, comma+sign, per-kind empty type). 5 LOW/SUSPECT
findings, all minor (repr of DEL, lone-`}` silent pass, comma+zeropad cosmetic, empty-precision dot,
over-broad isalpha). No memory-safety / resource-exhaustion / crash issues found (huge width/precision/
repetition all guarded; multibyte handling correct throughout).
