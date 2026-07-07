# v1.12.1 (audit loop) — Kirito-authored text/util modules (string, textwrap, base64, csv, tee, arg, semver)

Subsystem source: `src/kirito/stdlib_kimodules.hpp` (frozen-source Kirito modules).
Probe binary: `./build-debug/ki`

## LOG
- Read src sections: string_mod (478-543), textwrap (546-594), base64 (597-670), csv (673-759),
  tee (1006-1118), arg (1125-1267), semver (2572-2969).
- Probed all seven modules with build-debug/ki (`/tmp/p1..p11.ki`).
- RULED OUT / SOLID (verified correct behavior):
  - base64: enc/dec round-trip for 0/1/2/3/4 bytes, padding `=`/`==` correct; high bytes (chr255)
    round-trip; urlsafe alphabet; decode raises on bad char, embedded space, embedded newline, lone
    trailing char (bits==6), and truncated/non-canonical leftover bits. decode returns List of ints
    (matches its docstring). GOOD.
  - csv: quoted embedded comma/quote/newline, `""` escape, empty fields, trailing comma, ragged rows,
    CRLF, trailing newline, empty text, format() quotes fields needing it, parse(format(rows))==rows
    incl. fields with `,"`/newline/`\r`. GOOD.
  - textwrap: wrap/fill/indent basic + width<word (overflow, no break — by design), empty, width 0,
    huge width. GOOD.
  - string: capwords, constants (punctuation/whitespace), similarity identical/empty/disjoint/list,
    closest (+empty→None), fuzzymatch (+empty cand, cutoff boundary exact, empty query). GOOD.
  - arg: positional, --opt val, --opt=val, float conv, flags, short opts, neg-number as value AND as
    positional, bad-int/unknown-opt/unknown-short/missing-required all raise, extra→rest, -h→None.
    GOOD.
  - semver ranges (kpm-critical): caret (incl 0.x.y / 0.0.x special cases), tilde, x-ranges, `*`,
    `>`/`>=`/`<`/`<=` incl. partial round-up (`>1`→>=2.0.0, `<=1.2`→<1.3.0), space-after-op
    (`>= 1.2.0`), AND, OR (`||`), hyphen (incl. partial upper `1.0 - 2.3`→<2.4.0), prerelease gating
    (`^1.2.3` excludes `1.2.4-alpha`, includes `1.2.3-beta` under `^1.2.3-alpha`), maxsatisfying/
    minsatisfying/sort/rsort/validrange, unparseable range → satisfies False. ALL MATCH node-semver.
  - tee: fan-out reaches all copies + primary, write() returns len(data), writelines, list-of-copies,
    primary=None sink, tee_stdout context manager restores stdout on exit. GOOD.
  - semver.inc supports only major/minor/patch — matches docs (10-stdlib.md:1052). NOT a bug (the
    briefing's "pre" was a guess; docs never promise it).

## FINDINGS

### F1 [MED] semver.parse / valid under-validate: accept spec-invalid versions (silent)
- where: src/kirito/stdlib_kimodules.hpp:2589-2612 (parse), :2614-2620 (valid)
- repro:
  ```
  var sv = import("semver")
  sv.valid("01.2.3")     # => "01.2.3"   (leading zero in core)
  sv.valid("1.2.3-01")   # => "1.2.3-01" (numeric prerelease id with leading zero)
  sv.valid("1.2.3-bet@") # => "1.2.3-bet@" (non-[0-9A-Za-z-] prerelease char)
  sv.valid("1.2.3+")     # => "1.2.3+"    (empty build metadata)
  sv.valid("1.2.3+a..b") # => "1.2.3+a..b" (empty build identifier)
  ```
- actual: `valid()` returns the (uncleaned) string for versions node-semver rejects as null.
  expected: per semver.org §9-§11, leading zeros in numeric identifiers, non-`[0-9A-Za-z-]` chars,
  and empty prerelease/build components are INVALID → `valid()` should return None. The function
  already rejects empty *prerelease* ids (`len(p)==0` check) but not empty *build* ids, not leading
  zeros, and not the identifier charset.
- impact: `valid()` is the tag filter kpm uses; a malformed tag would be treated as installable.
  Low real-world impact (git tags are usually clean) but a documented-contract violation
  ("Returns the cleaned string, or None" — returns a string for invalid input).
- fix idea: in parse, reject numeric identifiers (core, and numeric pre/build ids) with a leading
  `0` and length>1; validate each prerelease/build identifier is non-empty and `[0-9A-Za-z-]`.

### F2 [LOW] semver.compare/major/minor/... throw a raw "cannot convert String to Integer" on >int64 version numbers
- where: src/kirito/stdlib_kimodules.hpp:2611 (Integer(nums[i]) in parse), reached via compare/major
- repro:
  ```
  var sv = import("semver")
  sv.compare("99999999999999999999.0.0", "1.0.0")  # THROW: cannot convert String to Integer: '99999999999999999999'
  ```
- actual: a version component exceeding int64 surfaces a low-level conversion error, not a
  semver-domain error. `valid()` on the same input correctly returns None (parse throws, caught),
  so the two disagree in error surface.
  expected: a clear "invalid version" domain error (or documented int64 ceiling).
- fix idea: pre-check numeric-id length / catch and rethrow as an invalid-version error in parse.

### F3 [LOW] textwrap.dedent measures indent by CHARACTER COUNT, not common-whitespace-prefix (diverges from Python on mixed tab/space)
- where: src/kirito/stdlib_kimodules.hpp:576-593
- repro:
  ```
  var tw = import("textwrap")
  tw.dedent("\t a\n \tb")   # => "a\nb"  (strips 2 chars from each, mixing a tab with a space)
  ```
- actual: dedent computes `minIndent = min(len(line)-len(lstrip(line)))` (a char count, tab==1) and
  slices `line[minIndent:]` — so a line indented `\t ` (tab+space) and a line indented ` \t`
  (space+tab) are both "dedented by 2", removing dissimilar whitespace.
  expected: Python's textwrap.dedent removes only the longest common leading-whitespace *string*; if
  lines share no common whitespace prefix it dedents nothing. This impl can silently realign code.
- fix idea: track the actual leading-whitespace substring and remove only the common prefix.
- severity LOW: docs don't pin exact semantics; still a silent-wrong-output edge on tab/space mixes.

### F4 [LOW] base64.decode silently ignores any input after the first `=` padding char
- where: src/kirito/stdlib_kimodules.hpp:644-646 (`if ch == "=": break`)
- repro:
  ```
  var b64 = import("base64")
  b64.decode("TWFu=garbage")  # => [77, 97, 110]   (trailing "garbage" ignored, no error)
  ```
- actual: decode stops at the first `=` and returns whatever decoded before it, ignoring trailing
  data — so clearly-corrupt input decodes without complaint.
  expected: arguably should validate that only padding follows (Python's b64decode with validate=True
  raises). Note: leniency is defensible (many decoders ignore); flagged as a silent-accept edge.
- fix idea: after the break, require every remaining char to be `=` and the total length a multiple
  of 4, else throw.

### F5 [LOW/SUSPECT] csv.parse silently tolerates unterminated quotes and mid-field quotes
- where: src/kirito/stdlib_kimodules.hpp:720-758
- repro:
  ```
  var csv = import("csv")
  csv.parse("\"abc")     # => [['abc']]   (unterminated quote, no error)
  csv.parse("a\"b\"c")   # => [['abc']]   (quotes mid-unquoted-field silently stripped)
  ```
- actual: an unclosed quote just consumes to EOF and is emitted; a quote appearing mid-field opens
  quote-mode and the quotes vanish.
  expected: RFC-4180-strict parsers reject an unterminated quoted field. By-design leniency is
  plausible, but it can mask corrupt CSV. SUSPECT.
- fix idea: if `inQuotes` is still true at EOF, throw an "unterminated quoted field" error.

### F6 [LOW/SUSPECT] arg: a short option colliding with `-h` is unreachable (help always wins)
- where: src/kirito/stdlib_kimodules.hpp:1215 (`-h`/`--help` checked before short-opt dispatch)
- repro:
  ```
  var arg = import("arg")
  var pr = arg.Parser()
  pr.option("host", "localhost")
  pr.parse(["-h"])   # prints usage, returns None — never sets host
  ```
- actual: any flag/option whose first letter is `h` can't be driven by its `-h` short form; `-h` is
  hard-wired to help. Also `_byshort` (first-letter match) makes any two options sharing a first
  letter collide on the earlier one (by design, but undocumented).
  expected: at least documented; ideally `-h` only means help when no option claims `h`.
- fix idea: consult `_byshort("h")` before treating `-h` as help, or reserve `-h` explicitly.

### F7 [LOW] tee: a throwing copy stream aborts the write before the primary is written
- where: src/kirito/stdlib_kimodules.hpp:1056-1061 (copies written first, no guard)
- repro:
  ```
  var io = import("io")
  var tee = import("tee")
  class Bad:
      var write = Function(self, d): throw "boom"
  var good = io.BytesIO()
  var t = tee.Tee(good, Bad())
  t.write("hi")            # THROW: boom
  good.getvalue()          # => ""   (primary never received the write)
  ```
- actual: because copies are written before the primary, a failing copy (e.g. a log file that errors)
  swallows the write to the real stdout — output is lost, not just the log.
  expected: this is the documented order ("saved before handed on"), so arguably by-design, but a
  logging tee suppressing console output on a log error is a footgun. SUSPECT/LOW.
- fix idea: optionally best-effort each copy (catch+continue) so the primary always gets the write,
  or document the fail-fast contract.

