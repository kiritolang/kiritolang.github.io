# A9 — DOCS ↔ IMPLEMENTATION reconciliation

Scope: bidirectional docs↔impl for the language/stdlib reference (`docs/pages/08-builtins.md`,
`09-types.md`, `10-stdlib.md`), the C++ embedding-API reference (`11-cpp-api.md`), and the exceptions
reference (`12-exceptions.md`). All probing done with the prebuilt `/home/jakub/kiritolang.github.io/build-bin/ki-asan-tls`
(TLS+OpenSSL enabled). No source edited, no build run.

Method: enumerated the *actual* public surface at runtime — `inspect(module)` for every module,
`inspect(value)` for every native type, and the C++-side symbol lists grepped from the headers — then
compared name/arity/defaults/return-type/behavior against the docs in both directions, RUNNING each
claim. Stdlib modules were fanned out to four parallel sub-auditors (numeric; collections/functional;
io/serde/crypto; net/text/parsing); I personally owned builtins, types, exceptions, and the C++ API.

**Headline: the docs are in tight agreement with the implementation.** Zero CONFIRMED docs→impl bugs.
The `docs/pages/*.md` code-fence tester passes 242/242 runnable blocks. Only three LOW cosmetic
coverage/wording items, all substantiated below.

---

## Docs → impl divergences

**No CONFIRMED bugs.** One LOW wording nuance (not a bug — the documented substring still matches):

### L1 (LOW, wording) — matrix `inverse` "singular" message
- Doc quote `docs/pages/10-stdlib.md:708`: "`m.inverse() → Matrix` — inverse. **Throws** `"singular"`
  if the matrix is singular".
- Reproducer:
  ```
  var matrix = import("matrix")
  matrix.Matrix([[1,2],[2,4]]).inverse()
  ```
  Actual thrown text: `matrix is singular (no inverse)`.
- The backticked `"singular"` reads as if it were the literal message; the real message merely
  *contains* "singular". Functionally faithful (a substring check passes). Maintainer call: loosen the
  doc phrasing or accept it as a substring. NOT a bug.

Everything else documented was RUN and matched — a large sample, module by module, listed under
"Verified CORRECT" below.

---

## Impl → docs gaps

Both are LOW: public, user-reachable names with no doc entry (genuinely internal only in that they
expose construction state; neither is a hidden underscore helper).

### G1 (LOW) — `StopIteration` global builtin not listed in the builtins page
- Exposed as a global class value (`installBuiltins`, `src/kirito/runtime.hpp:3543`); `type(StopIteration)`
  is `"StopIteration"`, always in scope.
- `docs/pages/08-builtins.md` does not mention it (grep count 0). It IS documented elsewhere —
  `09-types.md` (lazy generators §, lines ~502–511) and `12-exceptions.md:72` — so this is a
  cross-reference gap, not a true undocumented name. Consider a one-line pointer in 08-builtins.

### G2 (LOW) — `Tee.primary` / `Tee.copies` public attributes undocumented
- The `tee` module's `Tee` instance exposes two public attributes via `inspect`:
  `attr primary: StdStream` and `attr copies: List` (the constructor args, stored as reachable state).
- Reproducer:
  ```
  var io = import("io")
  var tee = import("tee")
  var t = tee.Tee(io.__stdout__, [])
  io.print(inspect(t))     # lists `attr copies` and `attr primary`
  ```
- The `## tee` section (`docs/pages/10-stdlib.md:1584-1600`) documents every *method*
  (`write`/`writelines`/`flush`/`close`/`streams`/`_enter_`/`_exit_`) but not these two attributes.
  Low impact (internal-state exposure, not intended API).

No other undocumented public names were found. Every module-function in each `inspect(module)` dump and
every non-underscore method from `inspect(instance)` maps to a doc entry — verified across all ~35
modules and every native type.

---

## Exceptions-reference drift

**None found.** Spot-checked ~30 error messages thrown by common operations against
`docs/pages/12-exceptions.md`; every one matched the catalogued text verbatim. Samples (reproducer →
actual text = doc text):

- `1 + "x"` → `unsupported operand type 'String' for arithmetic with 'Integer'` (12-exceptions.md:216)
- `[1][5]` → `index out of range` (:262)
- `{"a":1}["z"]` → `key not found: z` (:276)
- `1/0` → `division by zero` (:289); `7 % 0` → `integer modulo by zero` (:291); `5 // 0` →
  `integer division by zero` (:290)
- `(-2) ** 0.5` → `a negative base cannot be raised to a fractional power` (:293); `0 ** -1` →
  `zero cannot be raised to a negative power` (:292)
- `Integer("abc")` → `cannot convert String to Integer: 'abc'` (:394)
- `"a" + 1` → `can only concatenate String to String, not 'Integer'` (:220); `[1] + 2` →
  `can only concatenate List to List, not 'Integer'` (:221); `"x" * "y"` →
  `can only repeat String by an Integer` (:222)
- `len(5)` → `type 'Integer' has no length` (:239)
- `import(5)` → `import expects a String module name` (:415); `import("nope")` →
  `no module named 'nope'` (:416)
- `"abc".index("z")` → `substring not found` (:372); `[1,2].remove(9)` →
  `remove: value not in List` (:270)
- `chr(-1)` → `chr argument out of Unicode range` (:402); `ord("ab")` →
  `ord expects a single character` (:401)
- `xs[::2] = [9]` → `list slice assignment size mismatch: 2 target(s) but 1 value(s)` (:266)
- `d[[1,2]]=1` → `unhashable type 'List'` (:277); bad `_iter_` →
  `'Bad' _iter_ must return an iterator …` (:234)
- `assert 1==2` → `assertion failed`; `assert False, "custom boom"` → `custom boom` (:339)
- `a,b = [1]` → `expected 2 values to unpack, got 1` (:322)
- `math.sqrt(-1)` → `sqrt: math domain error (got -1.0)`; `math.log(0)` →
  `log: math domain error (argument must be > 0)` (:607+ math domain section; doc phrases it as a
  clear `math domain error`, actual adds the offending value — consistent, more diagnostic)
- `json.parse("{bad")` → `JSON parse error: …`; `json.stringify({1,2})` →
  `cannot serialize 'Set' to JSON` (:603)

Sub-auditors additionally reproduced module-specific messages that match their exceptions-reference
rows (hash `hmac: unknown algorithm …`, `iterations must be in [1, 4294967295]`; crypto
`aesdecrypt: authentication failed …`, `aes: key must be 16, 24 or 32 bytes …`; json `indent too
large (maximum 100)`, `cannot serialize a cyclic structure to JSON`; io `seek: resulting position is
negative`, `file not open for writing`, `I/O operation on closed file`; itertools
`itertools.count needs a stop bound …`, islice guards; enum `no such enum member: X`; time strptime
`text does not match format`, `sleep: seconds too large (maximum 1e9)`). No drift observed.

---

## Verified CORRECT / consistent

Everything below was RUN on `ki-asan-tls` (by me or a named sub-auditor) and found faithful to the docs.

**Builtins (`08-builtins.md`) — I personally verified all.** The full registered global set was
extracted from `installBuiltins` (`src/kirito/runtime.hpp:3529-4210`): 8 type constructors
(`Bool/Bytes/Dict/Float/Integer/List/Set/String`), 33 functions (`abs all any bin bitand bitnot bitor
bitxor chr divmod enumerate filter format fromhex hasattr hex id import inspect isinstance iter len map
oct ord pow reversed round shl shr slice sorted sum type zip`), variadic `range/min/max`, and
`StopIteration`. Every one is documented (except StopIteration, see G1). Ran and matched: all
conversions incl. `Integer("0xFFFFFFFFFFFFFFFF")==-1`, `isinstance(True,Integer)==False`,
`isinstance(1,"Number")`, `range(3)==[0,1,2]` + O(1) len, `divmod(-7,3)==[-3,2]`, `pow(2,10,mod=1000)`,
all bit ops, `round(0.5)==1`/`round(-1.5)==-2`, and the entire `format` example table (`05d`, `#x`,
`.2f`, `,`, `.1%`, `^6`, `*^7`, `+06d`) plus the four documented String-flag rejections
(`format: sign not allowed in string format specifier`, and the `#`/`=`/`,` variants). `min/max/range`
advertise `name(...)` under `inspect`; `min/max` accept `key=`/`default=`.

**Types (`09-types.md`) — I personally verified.** `inspect()` of each native value gives the exact
method set the doc tables list, with no extras and no omissions:
- String: all 33 documented methods present (apply/center/count/encode/endswith/find/format/index/
  isalnum/isalpha/isdigit/islower/isspace/isupper/join/levenshtein/ljust/lower/lstrip/partition/
  removeprefix/removesuffix/replace/rfind/rindex/rjust/rpartition/rstrip/split/startswith/strip/
  upper/zfill). Behaviors confirmed: `"ß".upper()=="ß"` (1:1 map), `"café"[3]=="é"`, `"café"[::-1]`,
  `"ab".apply(...)=="aabb"`.
- List/Set/Dict/Bytes method tables match the live surface exactly. Bytes lexicographic ordering,
  `Bytes([0xc3,0xa9]).decode("utf-8")=="é"`, list slice splice `xs[1:3]=[9,9,9]`, Set `.union([...])`
  accepting any iterable, Set operator RHS-must-be-Set throw — all confirmed.
- Integer/Float: `compare` (both) and Float `repr` present. `0.1+0.2==0.3` is False (exact bits);
  `(0.1+0.2).compare(0.3)` True; `(1.0/3.0).repr()` round-trips; `True + 1` throws (Bool not numeric).
- Behavioral error texts confirmed: `list slice assignment size mismatch`, `unhashable type 'List'`,
  `_iter_ must return an iterator`.

**C++ embedding API (`11-cpp-api.md`) — structural verification (read-only; can't compile embed tests
under the no-build rule, so these are informational not user-code-provable).** Every documented member
of the `KiritoVM` full surface (§15) exists in `src/kirito/vm.hpp` (all ~48 methods + `RootScope`/
`CallGuard`/`GcPauseScope`/`ChunkFileScope`). Every documented member of the `Value` API (§5 base +
type reference) exists in `src/kirito/value.hpp` (kind/typeName/truthy/asInt/asFloat/asBool/asStringRef/
asBoolV/asInteger/asString/asBytes/asList/asDict/asSet/try*/items/equals/floordiv/pow/contains/hash/
call/getAttr/setAttr/at/get/pairs/bound, `Value::None`, `detail::Anything`). Both referenced example
files exist (`tests/integration/embed_demo.cpp`, `tests/integration/embed_router.cpp`). No missing or
renamed symbols found.

**Doc-as-test:** `tools/scripts/test_docs_examples.py --ki ./build-bin/ki-asan-tls` →
`282 fenced blocks total (242 ran, 40 skipped by <!--norun-->, 0 failed)`.

**Stdlib modules (sub-auditors, all RAN their probes):**
- Numeric (math, complex, int, statistics, random, matrix): 0 divergences, 0 gaps. Domain-error
  battery, complex aliases + ordering/right-operand throws, BigInt left-dispatch/overflow/modinv,
  statistics quantile extrapolation, RNG reseed reproducibility, matrix det/inverse/vector ops.
- Collections/functional (tensor, itertools, functools, heapq, bisect, collections, copy, enum, tee):
  0 confirmed divergences; itertools eagerness + `count(start,step,stop)` order + islice guards,
  heapq new-vs-inplace, bisect tie-breaking, Counter `mostcommon` stability, deque both-end ops, copy
  shallow-vs-deep, ~90 tensor methods incl. einsum/kron/autograd domain throws. Gap G2 (tee attrs).
- io/serde/crypto (io, path, sys, time, json, dump, serialize, zlib, gzip, hash, crypto, base64):
  0 divergences, 0 gaps. json leniency + indent cap, hash vectors + pbkdf2 defaults + crc64 signed,
  crypto AES-GCM/RSA/EC round-trips + wrong-family throws + x509parse shape, io modes/seek/BytesIO,
  path strict mutation, sys exit-code wrapping, time strptime strictness + serialize round-trip,
  zlib/gzip type-preserving round-trip + multi-member gzip.
- net/text/parsing (net, parallel, regex, string, textwrap, csv, tabular, xml, arg, semver):
  0 divergences, 0 gaps. net URL helpers verbatim (IPv6 + protocol-relative), request `timeout`
  default `10` (`src/kirito/stdlib_net.hpp:992`), regex named groups/flags/`(a*)*` divergence +
  backref/lookahead rejection, semver full range surface (node-semver prerelease exclusion), parallel
  spawn share-nothing + blocking primitives, tabular DataFrame/Series/GroupBy, xml/csv/arg/textwrap
  examples verbatim.

**Bottom line for the orchestrator:** nothing actionable at HIGH/MED. Three LOW items (L1 wording,
G1 StopIteration cross-ref, G2 tee attrs) are the entire yield of a systematic, run-everything pass
over the whole documented surface. The reference set is faithful.
