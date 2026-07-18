# Coverage findings — serde/misc stdlib (dump, serialize, semver, arg)

Test: `tests/scripts/cov_serde_misc.ki` (177 top-level assertions, exit 0).
All behavior verified against `./build-bin/ki-release` before the assertions were written.
Impl: `src/kirito/stdlib_dump.hpp`, `stdlib_serialize.hpp`, `stdlib_serde.hpp` (shared graph
walk/rebuild), and `stdlib_kimodules.hpp` (semver + arg, both Kirito-authored).

## Verdict

No functional bugs found. Every documented behavior in `docs/pages/10-stdlib.md` for these four
modules was reproduced exactly, including the F08-2 dunder contract (`_bool_`/`_hash_`/`_eq_`
restored on a deserialized instance), cycle/shared-ref preservation, and all documented error
messages. Two low-severity observations below; neither is a doc delta or a memory-safety issue.

## Observations (informational, not bugs)

### 1. Trailing-data tolerance in both serde codecs (silent — minor)

Both `dump.loads` and `serialize.loads` stop reading immediately after the root id and IGNORE any
bytes that follow. A valid blob with extra trailing content loads successfully, returning the valid
prefix rather than reporting the malformed tail.

Minimal repro:

    var dump = import("dump")
    var serialize = import("serialize")
    var io = import("io")
    io.print(String(dump.loads(dump.dumps([1,2,3]) + Bytes([9,9,9,9]))))   # -> [1, 2, 3]  (no error)
    io.print(String(serialize.loads(serialize.dumps([1,2,3]) + " JUNK")))   # -> [1, 2, 3]  (no error)

Assessment: this is a lenient-parser choice, not a memory-safety hole (the reader is bounds-checked;
it simply does not require EOF after the root). It matches how many stream decoders behave (read one
value, leave the rest). Truncated / corrupt-within-the-record blobs DO error correctly (verified:
`truncated dump data`, `bad dump tag`, `serialized root id out of range`, etc.). Flagged only because
the task asked to surface any "corrupt blob that loads without error." No action recommended unless a
strict "consumed all input" guarantee is desired; if so it belongs in both `dumpfmt::decode` and
`serial::TextReader::decode` symmetrically.

### 2. semver leading-zero tolerance — confirmed documented non-bug

`semver.valid("01.02.03")` returns `"01.02.03"` and `semver.major("01.02.03")` returns `1`. Leading
zeros in numeric identifiers are accepted (normalized by `Integer()` at compare time, not in `raw`).
This is called out as a deliberate non-bug in `.audit/README.md` / the `_isident` comment in
`stdlib_kimodules.hpp`, and the docs do not promise rejection, so it is NOT a delta. Recorded here
only so the next auditor does not re-flag it.

## Coverage notes

- dump/serialize: None/Bool/Integer(incl. max/negative)/Float(incl. inf/-inf/NaN)/String(unicode/
  empty)/Bytes/List/Set/Dict(insertion order)/nested/Bytes-as-dict-key/cycles/shared refs; class
  instance dunder restoration; Function + class value round-trips; module reconnect-by-name;
  `_getstate_`/`_setstate_` (drops a transient field); save/load via temp files under
  `path.gettempdir()` (cleaned up). Hardening: wrong arg type, bad magic, empty/truncated, crafted
  unsupported-version / bad-tag / count-overflow / root-oob blobs, native-fn rejection,
  `_setstate_`-without-`_getstate_` rejection; text-codec bad header/count/number/tag/root.
- semver: parse (core/prerelease/build/raw, 0.0.0, leading-v, build-only, prerelease-only);
  clean/valid/major/minor/patch/prerelease; compare ordering (numeric-not-lexical, prerelease<release,
  numeric-prerelease<alphanumeric, build ignored, longer-prerelease-set); eq/neq/lt/lte/gt/gte;
  diff; inc (+ bad release); satisfies across caret/tilde/comparators/x-range/star/hyphen/OR/
  whitespace-operator/prerelease-pinning/unparseable-range; validrange (incl. git-ref rejection);
  sort/rsort/max|minsatisfying (original-string preservation, drops invalid); invalid-version errors.
- arg: chainable declaration; long/short/`=value`/default/None-default forms; Integer/Float/String
  coercion; flag present/absent; repeated-option-last-wins; flag-with-`=value`; positionals + `rest`;
  negative-number-stays-positional; undeclared-positionals->rest; usage text; `-h`/`--help`->None;
  hardening: missing required positional, unknown long/short option, missing option value, bad
  integer/float coercion.
