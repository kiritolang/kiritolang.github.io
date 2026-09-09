# A2 — Execution Engine Audit (v1.17.1)

Scope: `bytecode_vm.hpp`, `runtime.hpp` (operators / exceptions / control flow / subscript /
slice / getitem / setitem / iteration / calls / string ops / format), `dispatcher.hpp`
(single-VM parts only — the parallel worker pool is A-other's scope), `exceptions.hpp`,
`environment.hpp`, `function.hpp`.

Method: no compilation. Reproduced against `build-bin/ki-asan` (ASAN/UBSan,
`ASAN_OPTIONS=detect_leaks=0`, `ulimit -s 262144`, several runs under `KIRITO_GC_THRESHOLD=1`)
and `build-bin/ki-release`. Probes written to `/tmp/*.ki`. Fresh non-interned values (big ints,
freshly-built strings/containers) used throughout to avoid interned-small-int masking.

Overall: this is a heavily-audited engine and it held up. Extensive adversarial probing across
every hunt category produced **one** confirmed silent-wrong-result finding (narrow input range,
no UB, no crash). No ASAN/UBSan report, no crash, and no hang was triggered anywhere.

---

## Findings

### F1 — `Integer()` silently wraps a positive decimal string in [2^63, 2^64) to a negative int64
- **File:** `src/kirito/runtime.hpp:3628-3633` (the String→Integer conversion builtin).
- **Severity:** Medium (silent wrong result; violates the code's own documented contract). No UB,
  no crash. Narrow trigger range.
- **Status:** CONFIRMED (ki-asan and ki-release, identical).

**Reproducer**
```
Integer("9223372036854775807")   -> 9223372036854775807   # 2^63-1 = INT64_MAX  (correct)
Integer("9223372036854775808")   -> -9223372036854775808  # 2^63     WRONG (silent), expected: throw
Integer("10000000000000000000")  -> -8446744073709551616  # 10^19    WRONG (silent), expected: throw
Integer("18446744073709551615")  -> -1                     # 2^64-1   WRONG for a positive decimal
Integer("18446744073709551616")  -> throws "cannot convert String to Integer"  # 2^64 (correct)
```

**Root cause.** The magnitude is parsed with `std::stoull` (accepts anything < 2^64) and then
`static_cast<int64_t>(neg ? (~mag + 1ULL) : mag)` — an unchecked bit-cast. For a non-negative
base-10 value with `mag >= 2^63`, the bit-cast produces a negative int64 with no diagnostic. The
code comment (lines 3623-3627) explicitly states the intended contract: *"an unrepresentable value
FAILS FAST via stoull's out_of_range below rather than silently wrapping."* But `stoull` only
fails at `2^64`, so the entire window `[2^63, 2^64)` — exactly where a positive decimal stops fitting
in `int64` — is silently wrapped instead of rejected. The fail-fast intent is defeated for that
range.

**Note on the entanglement.** The bit-cast is deliberate and tested for the *bit-pattern* forms:
`Integer("0xFFFFFFFFFFFFFFFF") == -1` is asserted in `tests/scripts/cov_builtins.ki:61`,
`r5_regressions.ki:24`, etc. So a fix must preserve hex/oct/bin bit-pattern round-tripping and the
negative-decimal round-trip (`Integer(String(INT64_MIN))` — `neg`, `mag == 2^63`, correct). The
defect is specifically the **non-negative base-10** path: a suggested guard is, when `!neg` and the
resulting int64 is negative for a base-10 input (i.e. `mag > INT64_MAX`), throw the same
"cannot convert String to Integer" error rather than bit-casting. No test currently pins the
positive-decimal-overflow behavior (searched `tests/`), so this window is unintended collateral of
the shared magnitude parse, not a contracted behavior.

---

## Verified CORRECT (probed, no defect found)

**Integer / mixed arithmetic & overflow (no UB under UBSan)**
- INT64 boundary ops all wrap two's-complement via the `wadd/wsub/wmul/ipow/ifloordiv/imod` helpers
  (`runtime.hpp:153-182`): `MAX+1`, `MIN-1`, `MIN*-1`, `-MIN`, `MIN // -1`, `MIN % -1`, `MAX*MAX`,
  `2**63/64`. `MIN/-1` and `MIN%-1` special-cased away from the `INT64_MIN/-1` UB. No UBSan report.
- `abs(INT64_MIN)` returns `INT64_MIN` via `wsub(0,v)` (`runtime.hpp:3746`) — documented int64 wrap,
  no `llabs` UB. Correct-by-design (consistent with the fixed-int64 model).
- Div/floordiv/mod by zero (Integer, Float, BigInt) all throw structured, distinct messages
  ("integer division by zero" / "integer modulo by zero" / "float division by zero" / "division by
  zero"). `float %` uses `fmod` + sign correction (large-magnitude and inf-divisor cases correct).
- Exact Integer↔Float compare/equals/hash (`compareIntFloat`, no lossy double round-trip); NaN
  ordering imposes a strict weak order for sort (`kiLessThan`, `runtime.hpp:512`).
- `0**0 == 1`, `0.0**0 == 1.0`, `2**-1 == 0.5`, `0**-n` and negative-base-fractional-power throw.

**BigInt / Complex operator dispatch (VM interaction)**
- BigInt +,*,-,unary neg, `>`,`<`,`==`, `//0` (throws), `(-2)**3` correct. Left-dispatch enforced:
  `1 + BigInt` throws "unsupported operand type 'BigInt' for arithmetic with 'Integer'" (loud, no
  silent coercion), matching docs.
- Complex add, `/` by `0+0i` (throws "complex division by zero"), ordering `<` throws (unordered).

**String ops**
- `+` (String-only, else throws), `*` by Integer with the `kMaxRepeat` allocation guard
  (`runtime.hpp:471`), negative/zero count → "". UTF-8 length/index/negative-index/slice/reverse
  correct on multibyte (`"héllo→世界"`). Lexicographic `< <= > >=`.

**Slice / subscript / setitem (overflow-safe)**
- `resolveSlice` clamps with `std::clamp`; `rangeCount` divides the bounded span by `|step|` via
  unsigned negation (defined even for `INT64_MIN` step) — extreme bounds/steps
  (`a[INT64_MAX:]`, `a[::INT64_MIN]`, `"h"[INT64_MAX:0:-1]`) all correct, no overflow.
- List contiguous + extended slice assignment, size-mismatch error, negative indices, OOB throw.
- Dict/Set build + membership under a user `_hash_`/`_eq_`; missing-key throws "key not found".

**Iteration / generators (PEP-479, GC)**
- User `_iter_`/`_next_` generator drains correctly; `StopIteration` raised at `_next_`'s own frame
  ends iteration, but one leaking from a deeper call surfaces as an error (depth attribution,
  `runtime.hpp:2080-2094`) — verified not swallowed.
- Cyclic `_str_`/`_iter_` bounded (`[...]`, cycle guard, "recurses too deeply"); cyclic `==`/`<`
  bounded ("maximum comparison recursion depth exceeded").
- Full user-dunder suite (`_getitem_/_eq_/_hash_/_iter_`) correct under `KIRITO_GC_THRESHOLD=1` +
  asan with fresh non-interned keys/values in Dict/Set/for-loop — no UAF, no sweep of unrooted
  fresh elements.

**Exceptions / control flow**
- `finally` runs on normal exit, `return`, and `break`; nested try/finally reraise preserves the
  original site; an exception thrown in `finally` masks the original (correct). Bare `catch as e`
  promotes a KiritoError to a String value. `catch (KiritoError&)` ordered before
  `catch (KiritoThrow&)` at the VM boundary (`bytecode_vm.hpp:476-499`).

**Calls / recursion**
- Deep unbounded Kirito recursion → catchable "maximum recursion depth exceeded" (count guard).
- Deep recursion routed through a native higher-order builtin (`sorted(key=g)`) → caught via the
  native-stack-bytes guard (`vm.hpp:293-308`), no SIGSEGV (A04-1 path holds).
- Arg binding: too-many/unknown-kw/duplicate/missing all throw structured messages with
  hidden-leading `self` accounting; defaults evaluate per-call in the call scope; annotations
  enforced on args and return.

**format() / f-strings**
- Full f-string spec: fill+align, sign, `#`, `0`, width (with `kMaxRepeat` cap), `,` grouping,
  `.precision` (accumulated wide, capped, no int overflow), types `b/o/x/X/d/f/e/g/%/s`, sign-aware
  `=` alignment. String-only flags rejected loudly. `format(1.5,".999999999999f")` → precision guard.
- `str.format()` is intentionally positional-only (`{}` / `{n}`): a spec/named field
  (`{:.3}`, `{x}`) errors loudly ("format field must be an index"); OOB index errors loudly. Feature
  limitation, not a silent-failure defect (the misleading message for a `:`-spec is cosmetic only).

**Builtins**
- `divmod` (incl. `divmod(INT64_MIN,-1)`), `round` (documented half-away-from-zero; long-double
  scaling to avoid double-rounding; NaN/inf/out-of-range throw), `min/max` (empty throws),
  `hex/bin/oct/ord/chr`, list `* neg/0` and the repeat overflow guard.

---

## Nulls / honesty
- No second confirmed bug despite broad adversarial probing. I did **not** find any UAF, any
  ASAN/UBSan report, any crash, or any hang.
- `dispatcher.hpp` is almost entirely the parallel worker pool (explicitly out of scope); its
  single-VM surface is thin module-load plumbing and was not a fruitful target here.
- BigInt/Complex internals live in `stdlib_int.hpp`/`stdlib_complex.hpp` (stdlib scope); I audited
  only their interaction with the VM operator-dispatch path, which is correct.
