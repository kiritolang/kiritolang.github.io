# Doc-vs-impl verification: `json`, `serialize`, `dump`

Verified against `docs/pages/10-stdlib.md` (`## json`, `## serialize`, `## dump`) and the impl in
`src/kirito/stdlib_json.hpp`, `stdlib_serde.hpp`, `stdlib_serialize.hpp`, `stdlib_dump.hpp`.
Tests: `tools/tests/scripts/verify_json.ki` (71 asserts, `.expected` = `OK json`) and
`verify_serde.ki` (81 asserts, `.expected` = `OK serde`). Both green on `build-debug/ki`; the full
307-fixture stability harness runs 0 diverged.

## Discrepancies

**None.** Every documented behaviour of all three modules matched the implementation. No doc fixes and
no `src/` changes were needed.

## Pinned behaviours (current, matched by the tests)

### json
- Aliases hold: `json.dumps == json.stringify`, `json.loads == json.parse`.
- `dumps` exact output: `None→"null"`, `True/False→"true"/"false"`, `42→"42"`, `3.5→"3.5"`,
  `[]→"[]"`, `{}→"{}"`, `{"a":1}→'{"a": 1}'`, list separators are `", "`.
- Floats round-trip EXACTLY via the shortest form: `dumps(0.1+0.2) == "0.30000000000000004"`.
- Non-finite Floats round-trip as the JS-style bare tokens `NaN` / `Infinity` / `-Infinity` (not
  strict RFC-8259) and `parse` accepts them back.
- Control chars in a String are `\u00XX`/`\n`/`\t`/…-escaped.
- Non-string object keys emit canonical JSON tokens: a `True` key → `"true"`, a `None` key → `"null"`
  (via `writeJsonKey`), so they stay valid JSON (but do NOT round-trip back to Bool/None).
- `parse`: objects→`Dict`, arrays→`List`, int stays `Integer`, a `.`/`e` makes it `Float`; tolerant of
  surrounding whitespace; `\u` escapes and UTF-16 surrogate pairs decode to one code point; a lone
  (unpaired) surrogate becomes U+FFFD.
- Indent: `dumps(x, indent)` (positional or `indent=`) pretty-prints with `indent` spaces per level;
  `indent<=0` (the default `0`) is compact; empty containers stay `[]`/`{}` even when indented.
- Errors (all prefixed `JSON parse error: `): `unexpected character` (lone `}`, empty input),
  `expected ',' or ']'`, `expected string key` (unquoted key / trailing object comma),
  `invalid literal` (bad token), `trailing characters after JSON value`, `unterminated string`,
  `bad escape`.
- json has NO cycle/reference notion: a non-serializable value throws
  `cannot serialize '<type>' to JSON` (Set, Function, Bytes — nested ones too), and a self-referential
  List/Dict throws `cannot serialize a cyclic structure to JSON` (both compact and pretty paths) — it
  does NOT hang. No silent drops observed.

### serialize (text KSER1) + dump (binary KDMP) — one graph serializer, two codecs
- Output types: `serialize.dumps → String` starting `"KSER1 "`; `dump.dumps → Bytes` with magic
  `"KDMP"`. `dump.loads` also accepts a `String` carrying the same bytes.
- Both round-trip every built-in value type: None/Bool/Integer/Float (exact, incl. non-finite)/String
  (unicode, control chars, embedded spaces/newlines — text codec is space-tokenized yet correct)/
  List/Dict/Set/Bytes, empty containers included.
- User classes serialize by attributes (`_init_` is NOT re-run; post-init attributes are restored);
  the `_getstate_`/`_setstate_` protocol is honoured (transient fields dropped and recomputed).
- **Reference/cycle preservation:** a self-referential List/Dict reconstructs with `id()` identity
  (`id(c[1]) == id(c)`), does not hang; a shared child reachable by two paths stays ONE object after
  load (mutation through one path is visible via the other) — for plain containers AND class instances.
- Native VALUE types round-trip both formats: `Matrix`, `Vector` (a Matrix), `Complex`,
  `ComplexMatrix`, `DateTime` (value-equal by instant), `Random` (restores the EXACT generator stream),
  gradient-free `Tensor`; also nested inside containers with shared refs preserved.
- Content-hashed members survive: a `Set` of `Bytes` / of hashable instances, and a `Dict` keyed by
  `Bytes`/`DateTime` round-trip with correct `.contains`/lookup (the deferred pass-4 wiring works).
- **Resource-like natives throw a clear catchable error** (never crash):
  - serialize: `cannot serialize type '<T>' (define _getstate_/_setstate_ to make it serializable)`
  - dump: `cannot dump type '<T>' ...`  (`<T>` = Socket / File / Regex / …)
  - a grad-requiring `Tensor` throws `cannot serialize a Tensor that requires grad; call detach()
    first ...`; `detach()`-ing first round-trips.
- Corrupt/truncated blobs throw cleanly (no crash/hang):
  - text: `bad serialization header`, `unexpected end of serialized data` (empty/truncated),
    `corrupt serialized data: ...` (bad count / malformed number), `bad serialization tag '<c>'`,
    `serialized root id out of range`, `cannot deserialize: class '<name>' is not defined in this VM`.
  - binary: `bad dump header` (≥4 wrong magic bytes), `truncated dump data` (short of magic or body),
    `unsupported dump version`, `bad dump tag`, and `loads expects a Bytes (or String) of dump data`
    for a non-Bytes/String arg.
- `save`/`load` to a file round-trip both formats; a missing load path throws
  `could not open file for loading`. Temp files are created under `path.gettempdir()` and removed.

## Notes / API gotchas encountered (not bugs — pinned so future edits notice a change)
- `t.requiresgrad(True)` mutates in place and returns `None` (there is no `requiresgrad=` kwarg on
  `tensor.arange`; the ctor kwarg is on `Tensor(...)`).
- `Bytes` has no `.slice` method; slice via `b[a:b]`. `DateTime` is built with `time.make(y,m,d,...)`,
  not `time.datetime(...)` (which takes an optional single timestamp).
- Kirito has no `is` operator — identity is checked with `id(a) == id(b)`.
