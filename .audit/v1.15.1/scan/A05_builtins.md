# A05 — builtins (every builtin, every argument)

Round: v1.15.1. Agent: A05. Binary probed: `./build-debug/ki` (never built anything).

## Scope

Every Kirito builtin: `range, sum, min, max, abs, round, sorted, enumerate, zip, map, filter,
len, type, id, import, inspect, all, any, reversed, divmod, isinstance, hasattr, ord, chr,
bin, oct, hex, pow, bitand, bitor, bitxor, bitnot, shl, shr, format` + the constructors/
converters `Integer, Float, String, Bool, List, Set, Dict, Bytes`.

Sources: `src/kirito/builtins.hpp`, `src/kirito/runtime.hpp` (installBuiltins).

Method: probe the live binary; every claim carries pasted real output. Every probe re-run under
`--gc-threshold 1` asserting VALUES (not just "didn't throw").

## Findings

### A05-1: `range`'s resource guard is calibrated in BYTES but applied to an ELEMENT COUNT (~21 GB ceiling)  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/runtime.hpp:3180` (`if (count > kMaxRepeat) throw KiritoError("range too large")`), cap `src/kirito/common.hpp:159` (`kMaxRepeat = 256*1024*1024`).
- What: `kMaxRepeat` is the shared "~256 MB repetition/padding cap" (its own comment at `runtime.hpp:77` says so). For `String`/`Bytes` repetition the cap is applied per BYTE, so 256M ≈ 256 MB — correct. For `range` the same constant caps the ELEMENT COUNT, and each element costs ~80 B (a `Handle` in `list->elems` + a fresh non-interned `IntVal` + its arena slot). So the guard admits ranges up to ~21 GB. CLAUDE.md's contract is that "`range` [is] bounded (throw instead of OOMing)"; at this threshold the guard does not deliver that on ordinary hardware.
- Repro (measured, `ulimit`-bounded so as not to endanger the box):
```
$ cat /tmp/mem.ki
var io=import("io")
io.print(len(range(0, 5000000)))
$ ( ulimit -v 8000000; /usr/bin/time -f 'RSS=%M KB elapsed=%e s' ./build-debug/ki /tmp/mem.ki )
5000000
RSS=402288 KB elapsed=15.23 s
```
  402288 KB / 5e6 ≈ 80 B per element → `kMaxRepeat` (268,435,456) ≈ **21.5 GB** before "range too large" fires.
- Mitigating (important, and why this is LOW not MED): an allocation that *does* fail raises a **catchable** `std::bad_alloc`, not a crash — verified:
```
$ cat /tmp/oom.ki
var io=import("io")
try:
    var r = range(0, 20000000)
    io.print("built " + String(len(r)))
catch as e:
    io.print("caught: " + String(e))
$ ( ulimit -v 1000000; ./build-debug/ki /tmp/oom.ki )
caught: std::bad_alloc
```
  So the failure mode is graceful *when malloc fails*. The risk is Linux overcommit, where the OOM killer fires instead of malloc returning null.
- Impact: a script doing `range(0, 200000000)` (legal — under the cap) tries to allocate ~16 GB. On an overcommitting Linux box that is an OOM-kill, not a Kirito error. Low real-world likelihood (an accidental range that large is rare) — hence LOW.
- Proposed fix: give `range` its own count cap sized for its per-element cost, rather than borrowing the byte-sized `kMaxRepeat`. e.g. `kMaxRangeCount = 32*1024*1024` (~2.5 GB) in `common.hpp` beside `kMaxRepeat`, with a comment stating the per-element cost. Does not touch any documented contract (the error text "range too large" is unchanged; only the threshold moves).
- Proposed test: `tools/tests/errors/` — a `.ki`+`.experr` asserting `range(0, <cap+1>)` reports "range too large". Note the current suite has **no** test pinning the range cap at all (see coverage table).

### A05-2: `isinstance(v, type(v))` is true for every value EXCEPT a class value  [severity: LOW] [confidence: confirmed]
- Location: two sources of truth for "type name" — `Object::typeName()` (`class_value.hpp:66`, `ClassValue::typeName()` returns the class's own `name`), used by the `type` builtin (`runtime.hpp:3344`); vs `annotationTypeName(ValueKind)` (`runtime.hpp:1979-1993`, `Class -> "Class"`), used by `typeMatches`/`isinstance` (`runtime.hpp:2006`).
- What: the two disagree **only** for a class value, so the natural idiom `isinstance(v, type(v))` — which holds for Integer/Float/String/Bool/None/List/Dict/Set/instances — silently returns False when `v` is a class. Also `type(Foo) == type(Foo())` (both `"Foo"`), so `type()` cannot distinguish a class from its own instance.
- Repro:
```
$ ./build-debug/ki /tmp/p11.ki
type(Foo) => Foo
isinstance(Foo,'Class') => True
isinstance(Foo,'Foo') => False
isinstance(x,'Foo') => True
type(x) => Foo
isinstance(v,type(v)) for type=Integer => True
...  (Float/String/Bool/None/List/Dict/Set all True)
isinstance(v,type(v)) for type=Foo => False    <-- v is the CLASS Foo
isinstance(v,type(v)) for type=Foo => True     <-- v is an INSTANCE of Foo
```
- **Important — do NOT "fix" by changing `type()`.** The current behaviour is deliberately test-pinned:
  `tools/tests/scripts/r10_builtins.ki:47` — `ok(type(Dog) == "Dog", "type class value -> own name")`, and
  `r10_types.ki:710` — `ok(type(Vec(1, 2)) == "Vec", "type() reports class name")`. `ClassValue::typeName()`
  is also what makes error messages read `type 'Foo' has no attribute 'a'`. Changing either side would break
  a pinned contract — exactly the trap `.audit/README.md` warns about.
- Impact: low. A user writing the (reasonable, Python-valid) `isinstance(v, type(v))` guard gets a silent
  False for class values. No corruption; a wrong answer in a rare idiom.
- Proposed fix: **documentation only.** Note in the builtins reference under `type`/`isinstance` that
  `type()` reports a class value's *own name* (it is not a metaclass system), and that the supported way to
  ask "is this a class?" is `isinstance(v, "Class")`. No code change; no contract moves.
- Proposed test: a `.ki` case in `r10_builtins.ki` pinning the documented pair —
  `isinstance(Dog, "Class") == True` and `isinstance(Dog, "Dog") == False` — so the asymmetry is
  intentional-and-recorded rather than accidental. **This pair is currently untested** (only `type(Dog)` is).

### A05-3: `Bytes()` accepts ANY iterable, but its docs + its own error message say "a List of Integers"  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/bytes.hpp:329-340` (`makeBytes` falls through to a generic `o.iterate(vm)`); error text at `bytes.hpp:330`; docs quoted in CLAUDE.md ("`Bytes(x[, enc])` builds from a List of Integers, an Integer n (n zero bytes), a String, or a Bytes").
- What: the fallback accepts any iterable of Integers, so `Set`, `range`, and even a `Dict` (yielding its **keys**) are silently accepted. This is coherent with Kirito's iteration protocol (`List({1:2})` is `[1]` too), so the *code* is defensible leniency — but the contract text (both the docs and the thrown message) claims List-only, so a reader can't predict `Bytes({65:1, 66:2}) == b'AB'`.
- Repro:
```
$ ./build-debug/ki /tmp/p17.ki
Bytes(range(3)) => b'\x00\x01\x02'
Bytes(Set([65])) => b'A'
Bytes({65:1,66:2}) => b'AB'
```
- Impact: cosmetic/documentation. No corruption; the values produced are what the iteration protocol implies.
- Proposed fix: **documentation, not code.** Restricting to List would break `Bytes(range(n))`, which is a legitimate and useful call. Reword the docs + the `bytes.hpp:330` message to "an iterable of Integers (0..255), an Integer n, a String, or a Bytes".
- Proposed test: `r4_types.ki`-style case pinning `Bytes(range(3)) == b'\x00\x01\x02'` — currently **untested**.

### A05-4: `bytes.hpp:326` hardcodes `256ull * 1024 * 1024` instead of the shared `kMaxRepeat`  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/bytes.hpp:326` vs `src/kirito/common.hpp:159` (`kMaxRepeat = 256ull * 1024 * 1024`).
- What: a DRY break — `kMaxRepeat` exists in `common.hpp` precisely so this cap has one home, and line **126 of the very same file** already uses it with the comment `// shared cap (common.hpp)`. Line 326 re-spells the same literal. A future retune of `kMaxRepeat` would silently move one `Bytes` guard and not the other. This is the "fix applied to one copy and not the other" pattern the round brief calls out.
- Repro (static, exact):
```
$ grep -n '256ull \* 1024 \* 1024\|kMaxRepeat' src/kirito/bytes.hpp src/kirito/common.hpp
src/kirito/common.hpp:159:inline constexpr uint64_t kMaxRepeat = 256ull * 1024 * 1024;
src/kirito/bytes.hpp:126:            if (static_cast<uint64_t>(nrep) > kMaxRepeat / data.size())  // shared cap (common.hpp)
src/kirito/bytes.hpp:326:        if (static_cast<uint64_t>(n) > 256ull * 1024 * 1024) throw KiritoError("Bytes too large");
```
- Impact: no behaviour change today (the values are identical) — a latent divergence only.
- Proposed fix: `if (static_cast<uint64_t>(n) > kMaxRepeat)`. Zero behaviour change; `common.hpp` is already included via the same chain that gives line 126 `kMaxRepeat`.
- Proposed test: none needed (pure refactor); the existing `Bytes` size-guard behaviour is unchanged. Note that "Bytes too large" appears to have **no** test either (see coverage table).

