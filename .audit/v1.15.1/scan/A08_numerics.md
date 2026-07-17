# A08 — Numerics (Integer / Float / math / int.BigInt)

Round: v1.15.1. Binary probed: `./build-debug/ki`.

## Scope
- Integer/Float semantics in `src/kirito/runtime.hpp` (+ the numeric fast path in `applyBinaryOp`)
- `src/kirito/stdlib_math.hpp`
- `src/kirito/stdlib_int.hpp` (`BigInt`)
- Test coverage: `tools/tests/unit/test_numbers*.cpp`, `test_math*.cpp`, `test_bigint.cpp`,
  `tools/tests/scripts/*.ki`

## By-design table entries that bind me (from `.audit/README.md`)
- Float `==` is EXACT IEEE-754 (`0.1+0.2 != 0.3`, `NaN != NaN`); NaN Set/Dict keys are write-only.
- `math.trunc` / `round` return Integer.
- BigInt `/` of two huge equal operands → `nan` (via `inf/inf`) — lossy true-division contract.
- BigInt follows the reflected-operator rule: `BigInt(2)+3` works, `3+BigInt(2)` throws.

## Log

### Verified-good so far (details in Non-findings at the end)
- `// % ` sign rules for every negative combination: byte-identical to Python (Integer and Float).
- int64 two's-complement wraparound: well-defined at every boundary; `MIN // -1` and `MIN % -1` are
  explicitly guarded (`runtime.hpp:133,139`) — no SIGFPE, no UB.
- Exact IEEE-754 `==` invariants ALL hold, incl. the hard ones: `2^53+1 != 2^53.0` with correct
  trichotomy, `INT64_MAX != 2.0^63`, and Dict-key hashing agrees with `==` at 2^53.
- `.compare()` is stable under `--gc-threshold 1` with values outside the intern range and with every
  default omitted — the A19-1 bug class does NOT reproduce here.

---

### A08-1: `Integer.compare(other, 0.0, 0.0)` returns True for unequal Integers above 2^53  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/common.hpp:147` (`floatClose`) reached from the Integer `.compare` binding —
  both operands are converted to `double` before the comparison.
- What: `.compare` is documented (CLAUDE.md, Numerics bullet) as "close when
  `|a - b| <= max(rel_tol * max(|a|, |b|), abs_tol)`". With `rel_tol = 0.0, abs_tol = 0.0` that
  formula degenerates to exact equality (`|a-b| <= 0`). But because both Integers are first rounded
  to `double`, two int64s that differ by 1 above 2^53 collapse to the SAME double and `floatClose`'s
  `if (a == b) return true` short-circuit fires. So `.compare` reports "close" with **zero tolerance
  requested** for values that `==` and `<` correctly report as different. It breaks `.compare`'s own
  documented formula, and it is the one place in the numeric stack that is NOT exact — the language
  went to real trouble (`compareIntFloat`, runtime.hpp:107) to keep `==`/`<`/hashing exact
  precisely so they agree; `.compare` at zero tolerance is the odd one out.
- Repro:
```ki
var io = import("io")
var x = 4611686018427387904        # 2^62
var y = 4611686018427387905        # 2^62 + 1
io.print("x == y (exact ==)          :", x == y)
io.print("x < y                      :", x < y)
io.print("x.compare(y, 0.0, 0.0)     :", x.compare(y, 0.0, 0.0))
```
  Real output:
```
x == y (exact ==)          : False
x < y                      : True
x.compare(y, 0.0, 0.0)     : True
```
  Note the tell-tale asymmetry — it only bites when the two int64s round to the same double:
  `9223372036854775807.compare(9223372036854774784, 0.0, 0.0)` (differ by 1023) correctly → `False`.
- Impact: anyone using `.compare(a, 0.0, 0.0)` (or a tolerance small enough to be sub-ULP) as an
  "exact equality" check on large Integers — e.g. IDs, hashes-as-ints, nanosecond timestamps from
  `time.timens()`, financial integer cents — gets a false "equal". Narrow, but silently wrong.
- Proposed fix: in the Integer `.compare` binding, when BOTH operands are Integer, compute the
  difference exactly (`__int128` or the existing `wsub` + magnitude) and compare against the
  tolerance bound, instead of promoting to double. Keep the double path for any Float operand. This
  does NOT touch a documented contract — it makes `.compare` obey the formula it already documents,
  and it cannot change any result that is currently correct (only ones where the double round-trip
  already lost the distinction). Low risk.
- Proposed test: `tools/tests/scripts/` numeric spec (or `test_numbers.cpp`) —
  "compare with zero tolerance is exact above 2^53": assert
  `(2**62 + 1).compare(2**62, 0.0, 0.0) == False` while `(2**62).compare(2**62, 0.0, 0.0) == True`.
  Fails on the current build (returns True).

---

### A08-2: `math.prod` throws "result too large" for a product that is exactly 0  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/stdlib_math.hpp:222` (`intOverflow` is a sticky flag) — checked at line 229.
- What: `prod` tracks int64 overflow in a sticky `intOverflow` bool. Once an intermediate product has
  overflowed, a LATER element of `0` — which makes the exact mathematical result `0`, trivially
  representable as an Integer — still trips the check and throws `prod result too large for Integer`.
  The error message is simply false: the result is 0 and it fits. A product with a zero factor cannot
  overflow, so the sticky flag is unsound.
- Repro:
```ki
var io = import("io")
var math = import("math")
io.print(math.prod([4611686018427387904, 4, 0]))     # 2**62 * 4 * 0  -> exactly 0
```
  Real output:
```
prod result too large for Integer
```
  Python (`math.prod([4611686018427387904, 4, 0])`) → `0`. And Kirito itself gets it right when the
  zero arrives *first* via `start`: `math.prod([4611686018427387904, 4], 0)` → `0`. So the same
  mathematical quantity throws or succeeds purely on the ORDER of the zero — internally inconsistent.
- Impact: narrow (needs an overflowing prefix followed by a zero factor), but silently converts a
  correct answer into a spurious exception. Realistic for `prod` over a data column that contains a
  zero among large values.
- Proposed fix: a zero factor resets the running state, since 0 × anything is 0 and cannot overflow —
  in the Integer branch of the element loop, when `v == 0` set `n = 0` and clear `intOverflow`.
  Subsequent `__builtin_mul_overflow(0, v, &n)` cannot re-trip. No documented contract is touched
  (the "Integer prod throws on overflow" rule stays; this only stops it firing when there IS no
  overflow).
- Proposed test: `tools/tests/scripts/` math spec — "prod with a zero factor after an overflowing
  prefix is 0, not an overflow": assert `math.prod([2**62, 4, 0]) == 0`. Fails on the current build.

---

### A08-4: BigInt/Integer Dict+Set key equality is ASYMMETRIC — a Dict holds two `==`-equal keys  [severity: HIGH] [confidence: confirmed]
- Location: `src/kirito/collections.hpp:116` (`probeBucket` — the single shared bucket probe for Dict
  AND Set). Root cause is the raw `Object::equals` call; contrast `runtime.hpp:1650-1654` (`kiEquals`)
  which ALREADY has the symmetric fallback this needs.
- What: CLAUDE.md's `int` bullet states of BigInt: "**it hashes equal to an equal native Integer
  (shared Dict/Set bucket)**". It does not. The hashes DO match, but key equality is asymmetric, so
  whether the two collapse into one entry depends on **insertion order** — and in the bad order the
  Dict ends up holding two distinct keys that are `==` to each other and *print identically*. That
  breaks both the documented contract and the fundamental Dict/Set invariant (no two `==` keys).

  Mechanism:
  - `IntVal::equals` (`runtime.hpp:272`) only recognizes kinds `Integer`/`Float`. A `BigIntVal` is a
    `NativeClass`, so `IntVal::equals(BigIntVal)` → **false**.
  - `BigIntVal::equals` (`stdlib_int.hpp:528`) explicitly recognizes `Integer`/`Bool` → **true**.
  - `probeBucket` compares **stored.equals(probe)** only. Stored Integer + probe BigInt → miss →
    duplicate insert. Stored BigInt + probe Integer → hit → dedupe. Hence order-dependence.
  - The `==` *operator* is symmetric (it routes through `kiEquals`, which retries the other side),
    which is why `1000 == BigInt(1000)` is True while the Dict disagrees — the two disagree.
- Repro:
```ki
var io = import("io")
var i = import("int")
var b = i.BigInt(1000)
io.print("BigInt(1000) == 1000        :", b == 1000)
io.print("1000 == BigInt(1000)        :", 1000 == b)
var d = {}
d[1000] = "native-first"
d[b]    = "bigint-second"
io.print("len(d)                      :", len(d), "  (contract says 1)")
io.print("d                           :", d)
var d2 = {}
d2[i.BigInt(2000)] = "bigint-first"
d2[2000]           = "native-second"
io.print("len(d2)                     :", len(d2))
io.print("1000 in {BigInt(1000)}      :", 1000 in {b})
io.print("BigInt(1000) in {1000}      :", b in {1000})
io.print("len({5000, BigInt(5000)})   :", len({5000, i.BigInt(5000)}))
io.print("len({BigInt(5000), 5000})   :", len({i.BigInt(5000), 5000}))
```
  Real output (identical with and without `--gc-threshold 1`):
```
BigInt(1000) == 1000        : True
1000 == BigInt(1000)        : True
len(d)                      : 2   (contract says 1)
d                           : {1000: 'native-first', 1000: 'bigint-second'}
1000 in {BigInt(1000)}      : True
BigInt(1000) in {1000}      : False
len({5000, BigInt(5000)})   : 2
len({BigInt(5000), 5000})   : 1
```
  Note `{1000: 'native-first', 1000: 'bigint-second'}` — a visibly corrupt Dict. And `in` contradicts
  itself depending on which side holds the BigInt.
- Impact: any code mixing BigInt and native Integer keys — a memo/cache Dict in a bignum algorithm
  (`modpow` memo, factorial cache, a sieve's seen-set) is the canonical case, and mixing is natural
  because BigInt arithmetic accepts native Integers as operands. Symptoms: duplicate cache entries, a
  cache that never hits, `in` returning False for a value that is `==` to a member, and a Dict whose
  `len()` disagrees with its rendered contents. Silent, order-dependent wrong answers — no exception.
- Proposed fix: give `probeBucket` the same symmetric retry `kiEquals` already documents and uses
  (single-sourcing the *rule*, even though the VM-less `probeBucket` cannot call `kiEquals` itself —
  `collections.hpp` is compiled before `runtime.hpp` and only has an `ObjectArena`, which is all the
  retry needs):
```cpp
for (std::size_t i = 0; i < bucket.size(); ++i) {
    const Object& stored = arena.deref(keyOf(bucket[i]));
    if (stored.equals(arena, key)) return static_cast<std::ptrdiff_t>(i);
    // Cross-type equality must stay symmetric (mirrors kiEquals, runtime.hpp): a stored Integer
    // doesn't know about BigInt, but a BigInt knows about Integer.
    if (stored.kind() != key.kind() && key.equals(arena, stored))
        return static_cast<std::ptrdiff_t>(i);
}
```
  Contract safety — checked against the false-positives table:
  - **NaN keys stay write-only.** The retry is gated on `stored.kind() != key.kind()`; two Floats have
    the SAME kind, so the NaN path is untouched. The by-design write-only-NaN behaviour is preserved.
  - **Complex stays unhashable**, so it is never a key and is unaffected (the table's rationale for
    making it unhashable — "a subtly-wrong hash corrupts Set/Dict" — is exactly the trap BigInt fell
    into by choosing hashable; this fix is what makes that choice sound).
  - Integer↔Float keys already match symmetrically on the first check, so the retry is a no-op there.
  - Blast radius is precisely "a hashable native type that recognizes a builtin it isn't recognized
    by" = BigInt today.
  Fix in `probeBucket` (not in `BigIntVal`) because it is the ONE shared probe for Dict and Set — one
  edit fixes both, and it also inoculates any future hashable native that recognizes a builtin.
- Proposed test: `tools/tests/scripts/` (an `int`/BigInt spec) named for the symptom — "a BigInt and
  an equal Integer share one Dict/Set bucket in EITHER insertion order":
  assert `len({5000, i.BigInt(5000)}) == 1` and `len({i.BigInt(5000), 5000}) == 1`;
  a Dict written native-first then BigInt-second has `len == 1` and the second write overwrites;
  and `i.BigInt(1000) in {1000}` is True (currently False). Plus a pinning assertion that
  `len({math.nan, math.nan}) == 2` still holds, proving the fix didn't disturb write-only NaN keys.
  The Set/`in` assertions fail on the current build.

---

### A08-3: `.audit/README.md`'s false-positives table MISDESCRIBES the v1.12 A11-1 verdict on `math.trunc`  [severity: MED] [confidence: confirmed]
- Location: `.audit/README.md:67` (the false-positives table, first row).
- What: the table row reads "`math.trunc(x)` / `round(x)` return an **Integer**, not a Float (v1.12
  A11-1) | by design". That is **backwards for `trunc`**. The original v1.12 A11-1 finding
  (`.audit/v1.12/agents/A11.md:19`) is titled "`math.trunc` returns Float while `floor`/`ceil` return
  Integer" — i.e. the complaint was that trunc returns **Float**. The table inverted it, apparently
  conflating `math.trunc` with the builtin `round` (which genuinely does return Integer).
- Repro:
```ki
var io = import("io")
var math = import("math")
io.print("type(math.trunc(3.7)) =", type(math.trunc(3.7)), "value:", math.trunc(3.7))
io.print("type(math.floor(3.7)) =", type(math.floor(3.7)))
io.print("type(round(3.7))      =", type(round(3.7)), "value:", round(3.7))
```
  Real output — trunc is a **Float**, round is an Integer:
```
type(math.trunc(3.7)) = Float value: 3.0
type(math.floor(3.7)) = Integer
type(round(3.7))      = Integer value: 4
```
  The code (`stdlib_math.hpp:83`, registered via the Float-returning `unary` helper), its own file
  comment ("floor/ceil/factorial/gcd return Integer" — trunc pointedly absent),
  `docs/pages/10-stdlib.md:635` (`trunc(x: Number) → Float`) and `inspect(math)`
  (`trunc(x: Number) -> Float  [native]`) ALL agree: trunc → Float. Only the audit table disagrees.
- Impact: this is exactly the failure mode this round's README warns about — a wrong "all clear" that
  never gets audited. A future agent reads the table, "knows" trunc returns Integer, and either
  (a) dismisses a real trunc finding as a known false positive, or (b) "restores" the documented
  Integer return — a silent breaking change to a Float-returning public API. The table is consulted
  as authoritative *before* probing, so the error propagates.
- Proposed fix: correct the row to state the ACTUAL v1.12 verdict — the builtin `round(x)` returns an
  Integer by design, and `math.trunc(x)` returns a **Float** by design (deliberately: unlike
  floor/ceil it has no `toInt64Checked` guard, so `trunc(1e300)` → `1e+300` returns cleanly where
  `floor(1e300)` throws "result out of Integer range" — the permissiveness is the point). Split the
  row so the two functions are not conflated. NOTE: this is a fix to the AUDIT RECORD, not to code —
  the code is correct as-is and must not be changed.
- Proposed test: none needed for the doc fix itself, but the v1.12 coverage gap it flagged is still
  open — no test asserts `type(math.trunc(x))`. Add to the math spec:
  `assert type(math.trunc(3.7)) == Float` and `assert type(math.floor(3.7)) == Integer`, pinning the
  deliberate divergence so it can't drift again (see the coverage table below).

