# Coverage findings — scalar types + String/Bytes methods (v1.16.1)

Area: `docs/pages/09-types.md` — None / Bool / Integer / Float / String / Bytes.
All observations from `./build-release/ki` (Opus audit). Test: `tests/scripts/cov_scalars_strbytes.ki`.

## Silent-error / lax-argument findings (hardening gaps)

These string/bytes methods accept and **silently ignore** surplus positional arguments
instead of raising an arity error (contrast: `compare()`, `find()`, `center()`, `replace()`
all correctly reject wrong arg counts). Captured in the test as the current (lax) behavior.

- `"hi".upper("x", "y")` → `"HI"` (extra args ignored; `upper()` takes none).
- `"HI".lower(5)` → `"hi"` (extra arg ignored).
- `"1".isdigit(9)` → `True` (extra arg ignored; same class for the other `isX` predicates).
- `Bytes([...]).hex(1)` → normal hex (extra arg ignored).
- `"hi".strip("h", "x")` → `"i"` — strips using the **first** `chars` arg only; the 2nd
  positional is silently ignored rather than an arity error.

Not crashes and not wrong results, but inconsistent with the strict-arity methods; worth a
hardening pass to reject surplus positionals uniformly.

## Doc-vs-impl / task-vs-impl deltas

- **`rsplit` and `splitlines` do not exist.** The task brief lists them, but they are absent
  from both the docs table (09-types.md only lists `split`/`join`) and the impl:
  `"a,b,c".rsplit(...)` → `type 'String' has no attribute 'rsplit'`; same for `splitlines`.
  Impl and docs agree (feature simply not present). Tests assert the no-attribute error.
- **`Integer(str, base)` takes no base argument.** `Integer("ff", 16)` →
  `Integer() takes at most 1 positional argument(s) but 2 given`. Base is auto-detected from
  the string prefix instead: `Integer("0xff") == 255`, `Integer("0o17") == 15`,
  `Integer("0b101") == 5`. Docs don't claim a base arg, so consistent.
- **`round()` breaks ties away from zero, not banker's (Python-style) rounding.**
  `round(0.5)==1`, `round(1.5)==2`, `round(2.5)==3`, `round(-2.5)==-3`. Python's `round`
  would give `0`, `2`, `2`. Docs don't specify tie-breaking; recorded so the choice is
  captured. (round is a builtin; only lightly touched here since Float docs reference it.)
- **`String(Bytes(...))` yields the bytes *repr***, not a decoded string:
  `String(Bytes([72,105])) == "b'Hi'"`. To decode use `.decode()`. Undocumented; noted.
- **`str.split(None)`** behaves like the no-arg form (splits on whitespace runs). Undocumented
  but reasonable; `split("")` correctly throws `empty separator`.
- **`startswith`/`endswith` accept only a String prefix**, not a tuple/list of options
  (Python allows a tuple): `"hi".startswith(["h","x"])` → `startswith requires a String`.
  Consistent with the docs wording ("prefix `p`").

## Confirmed-correct highlights (no issue)

- `abs(-9223372036854775808)` stays negative (documented most-negative edge).
- Integer overflow wraps two's-complement; source literals past 2^63 wrap; `0x1_0...0`→`0`.
- `1/0`→`division by zero`; `1//0`→`integer division by zero`; `1%0`→`integer modulo by zero`;
  `(-2)**0.5`→domain error; `0**-1`→domain error. No silent NaN.
- Float `==` is exact; `nan==nan` False, `inf==inf`/`0.0==-0.0` True; `1.0==1` True and they
  share one Dict key, while `True`/`1`/`1.0` split: `True != 1` and `{False:0, 0:1}` keeps
  **two** keys (Bool is not Integer), but `{1.0:"a", 1:"b"}` collapses to one.
- `.compare` (Integer & Float) honors `rel_tol`/`abs_tol` kwargs, rejects non-number args and
  unknown kwargs; `nan.compare(...)` always False.
- String indexing/slicing is by code point (astral `"a😀b"` has len 3, `[1]` is the emoji),
  negative/oob indices throw `index out of range`, `[::0]` throws `slice step cannot be zero`.
- `encode`/`decode` reject bad codecs and malformed/overlong/out-of-range bytes; latin-1 is the
  lossless byte↔codepoint bridge.
- `fromhex` rejects odd length, split nibbles, and non-hex digits.
- Strings/Bytes are immutable (`s[0]=...` → `does not support item assignment`).
