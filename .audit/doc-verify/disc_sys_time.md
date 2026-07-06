# Doc-vs-impl verification: `sys` and `time` stdlib modules

Verified against `docs/pages/10-stdlib.md` (`## sys`, `## time`) + `src/kirito/stdlib_sys.hpp` /
`src/kirito/stdlib_time.hpp`. Runner: `build-debug/ki`. Tests:

- `tools/tests/scripts/verify_sys.ki` (+ `.expected` = `OK sys`) — 49 asserts.
- `tools/tests/scripts/verify_time.ki` (+ `.expected` = `OK time`) — 74 asserts.

**Result: NO discrepancies.** Every documented function/attribute/method behaves as the docs
describe. No `src/` changes needed; nothing recorded in `DISCREPANCIES.md`. Behaviours pinned below.

## sys — pinned behaviour

- `platform` = `"linux"` here; one of `{linux, darwin, windows}`. `arch` = `"x64"`; one of
  `{x64, arm64, x86, unknown}`. Both are `String` values (not functions).
- `version` = `"1.12.0"` (== `kVersion`, == `ki --version`); a dot-separated semver-shaped String,
  `>= 3` components, numeric major/minor. **PIN:** current release-label is `1.12.0`.
- `getenv(name, default=None)`: missing var → `None` (or `default`); accepts keyword args and a
  non-String `default` (returned as-is). Non-String `name` throws.
- `setenv`/`unsetenv` both return `None`. Non-String name throws.
- **PIN (POSIX):** `setenv(K, "")` keeps the var SET with an empty value — `getenv(K) == ""`, not
  `None`. Docs note Windows treats `""` as unset; the POSIX path (tested here) does not.
- `environ()` is a **function** returning a `Dict`; reflects live `setenv`/`unsetenv`.
- `traceback()` → `String`; `""` before any error is unwound, non-empty afterwards (starts with
  `"Traceback (most recent call last):"` and embeds the source path — so tests assert on it, never
  print it).
- `shell(command, cwd=None, input="", timeout=None)` and
  `createprocess(args, cwd=None, input="", timeout=None)` both return `{code, stdout, stderr}`
  (Integer code, String out/err). Nonzero child exit is a normal result (code propagated, not
  thrown). `input` feeds stdin; `cwd` sets the working dir. `createprocess` passes argv verbatim (no
  shell → shell metachars like `a*b` are literal).
- **PIN — error messages (exact substrings):**
  - missing program: `failed to start '<name>': No such file or directory`
  - empty argv: `createprocess: args must be a non-empty List (the program and its arguments)`
  - non-List args: `createprocess: args must be a List of Strings (the program and its arguments)`
  - non-String element: `createprocess: each argument must be a String ...`
  - timeout (both fns): `process timed out`
- `createprocess`/`shell` with `timeout=0.2` on a `sleep 3` child kills+throws in ~0.2s (fast).
- `sys.exit` is documented but NOT exercised (it terminates the process via `std::_Exit`).

## time — pinned behaviour

- Clocks: `time()`→Float, `timens()`→Integer, `monotonic()`→Float, `perfcounterns()`→Integer.
  `monotonic`/`perfcounterns` are non-decreasing across two reads.
- `sleep(seconds)`→None; a non-positive value is a silent no-op (guarded `secs > 0`); non-number
  throws.
- `now()`/`datetime()`→DateTime; `datetime(epoch)` from Integer or Float seconds.
- **DateTime fields** are Integer attributes (no parens): year/month/day/hour/minute/second,
  `weekday` (**0=Sunday … 6=Saturday**, C convention — epoch 1970-01-01 → 4/Thursday),
  `yearday` (1-based), `timestamp` (epoch seconds).
- **PIN — fixed instants:** `datetime(0)` iso `1970-01-01T00:00:00`, weekday 4, yearday 1.
  `make(2020,1,1)` epoch `1577836800`, weekday 3 (Wed), iso `2020-01-01T00:00:00`.
- Methods: `iso()` / `isoformat()` (alias), `format(fmt)` (full strftime; `""`→`""`),
  `add(sec)` / `sub(sec)` (Float delta truncated to int64; `add(0)` no-op),
  `diff(other)` = signed `self-other` seconds.
- **Equality + hashing by instant:** same-epoch DateTimes are `==`, hash equal, usable as Dict
  keys / Set members. Serializable through both `serialize` and `dump` (round-trip preserves epoch).
- **PIN — `make` normalizes** (docs "C mktime-style", impl uses pure civil arithmetic):
  `make(2020,13,1)`→`2021-01-01`, `make(2020,1,32)`→`2020-02-01`, `make(2020,0,1)`→`2019-12-01`,
  `make(2020,1,1,24)`→`2020-01-02`. `make` does NOT throw on out-of-range date fields.
- **PIN — `strptime` is STRICT** (opposite of `make`): supports only `%Y %m %d %H %M %S %%`; a
  literal/format mismatch, out-of-range field (month 13, hour 25, `2024-99-99`), unconverted
  trailing input (`2024-01-01XYZ`), or any unsupported directive (`%Q`) all throw
  `strptime: text does not match format`. Partial format uses sane defaults
  (`strptime("2020","%Y")` → `2020-01-01T00:00:00`).
- **PIN — hardening messages (exact substrings):**
  - epoch year outside [-9999, 9999]: `... is out of representable range` (e.g. `datetime(253402300800)`
    → year 10000; `datetime(-400000000000)` → year -10706; `make(20000,1,1)` too).
  - `datetime(NaN)`: `datetime: cannot convert NaN to a timestamp`; `datetime(inf)`:
    `datetime: cannot convert infinity to a timestamp`.
  - `make` component beyond ±2e9: `make: date component out of range`.
  - `add`/`sub` non-number: `add/sub expects a number of seconds`.
  - `diff` non-DateTime: `diff expects a DateTime`. `format` non-String: `format expects a String`.
  - **Immutability guard:** calling `dt._setstate_(n)` on an already-built DateTime throws
    `DateTime _setstate_: cannot re-initialize an established DateTime (it is immutable once built)`
    — `_setstate_` only serves the deserializer's `alloc(empty)→_setstate_` path.
