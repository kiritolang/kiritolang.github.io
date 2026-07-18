# Coverage findings — text-serialization stdlib (json, csv, base64, xml)

Test: `tests/scripts/cov_serde_text.ki` (+ `.expected`). 169 `assert`s, all verified against
`./build-release/ki` before being written. Round-trip / edge / hardening coverage of every
documented function in the json, csv, base64 and xml doc sections.

## Deltas / bugs / silent-errors

### 1. json.stringify cycle error message: doc-vs-impl delta
`docs/pages/10-stdlib.md` (json section) says a value with no JSON form — "a `Set`, a function, a
class/instance without a JSON mapping, or a structure containing a **cycle** — throws
`cannot serialize '<Type>' to JSON`." That message is correct for Set/function/instance, but a
**cycle** actually throws a *different* message:

```
$ printf 'var c=[1]\nc.append(c)\ndiscard import("json").stringify(c)\n' | ./build-release/ki /dev/stdin
cannot serialize a cyclic structure to JSON
```

Impl: `src/kirito/stdlib_json.hpp:286,325` throw `"cannot serialize a cyclic structure to JSON"`.
The test asserts the real substring `cyclic structure`. Minor doc inaccuracy — the doc implies the
cycle case shares the `'<Type>'` wording, which it does not. (No code change; recorded per "record
doc-vs-impl deltas, don't bend the test".)

### 2. base64.decode silently drops everything after the first `=` (SILENT ERROR)
`decode` breaks on the first `=` and never validates the remainder, so trailing garbage (including
otherwise-invalid characters) past the padding is accepted without error:

```
# via a script file / /dev/stdin (ki has no -e flag):
$ printf 'io=import("io")\nb=import("base64")\nio.print(String(b.decode("TWFu====garbage")))\n' | ./build-release/ki /dev/stdin
[77, 97, 110]
# and b.decode("TWE=X") -> [77, 97]  (invalid trailing char after '=' silently ignored)
```

The module otherwise takes hardening seriously (it rejects a lone trailing char and non-zero leftover
bits — see `stdlib_kimodules.hpp:668-675`), so accepting invalid trailing bytes after `=` is
inconsistent: `decode("TWFu=@#!$")` should arguably raise `invalid base64 character`, matching the
non-padded path. Impl: `stdlib_kimodules.hpp` `decode`, the `if ch == "=": break` short-circuits
before any trailing validation. The test pins the CURRENT behavior with an explicit "SILENT ERROR"
comment so a future fix trips the assertion rather than passing silently.

## Scope clarifications (not deltas)

- The csv **module** only exposes `parse` / `parserow` / `format` / `formatrow`, and does **no** type
  inference (fields are always `String`) — verified. The header-handling, type inference, ragged-row
  padding/throwing, custom delimiter, and the `readcsv` contract mentioned in the coverage brief are
  features of the **`tabular`** module (which builds on csv), not of `csv` itself. They are out of
  scope for the csv doc section and were not tested here.
- csv `parse` edge results confirmed: trailing `\n` yields no phantom row; a blank line yields a
  one-empty-field row `[""]`; empty text yields `[]`; a lone `\n` yields `[[""]]`; CR is dropped
  (CRLF == LF); ragged rows are preserved verbatim (no pad, no throw).
- xml parser is lenient by contract: malformed markup (`<a><b></a>`, unclosed tags, unquoted attrs,
  never-closed comment, `<<>>`) is tolerated and never throws; unknown/out-of-range/malformed entities
  are kept verbatim. All confirmed.
