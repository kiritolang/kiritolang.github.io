# A07 — String + Bytes: every method, every argument

Round: v1.15.1
Scope: `src/kirito/bytes.hpp`, the String type-method surface in `runtime.hpp`, the format
mini-spec, f-string rendering, encodings (utf-8 / latin-1 / ascii).

Known false positives (from `.audit/README.md`, NOT to be re-litigated):
- `String.format()` ALLOWS mixing auto `{}` and manual `{N}` — pinned by design.
- `io.write`/`io.print` stringify a `Bytes` (repr) rather than writing raw bytes — intentional.

Method: probe `./build-debug/ki` live; every claim carries real pasted output. Every probe is
ALSO re-run under `--gc-threshold 1` (the write-barrier soak that found A19 last round).

---

## Progress log

- [ ] read bytes.hpp
- [ ] read String type-methods in runtime.hpp
- [ ] format mini-spec
- [ ] f-string rendering
- [ ] live probes
- [ ] test-coverage table

(appending as I go)

## Verified clean so far (all probes ALSO run under `--gc-threshold 1`)

**GC soak result** — the headline check for this round. A soak using values deliberately OUTSIDE the
interned small-int range (positions/counts/distances > 256, 1000-byte Bytes, astral text, allocating
`apply` callbacks) passes identically with and without the barrier soak:

```
$ ./build-debug/ki /tmp/a07_soak.ki
ALL SOAK CHECKS PASSED
$ ./build-debug/ki --gc-threshold 1 --gc-stats /tmp/a07_soak.ki
ALL SOAK CHECKS PASSED
gc-stats: minor=4352 (137.878 ms)  major=622 (146.757 ms)  live=680  young=1  capacity=2661
```
And every other probe script is byte-identical under the soak:
```
a07_fmt1: IDENTICAL under --gc-threshold 1
a07_fmt2: IDENTICAL under --gc-threshold 1
a07_str:  IDENTICAL under --gc-threshold 1
a07_bytes: IDENTICAL under --gc-threshold 1
a07_enc:  IDENTICAL under --gc-threshold 1
```

### The `bytes.hpp` apply unrooted-`makeInt` question (brief's specific ask) — SAFE, no sibling
`bytes.hpp:294` passes `vm.makeInt(c)` into a user callback unrooted:
```cpp
std::array<Handle, 1> args{vm.makeInt(c)};
```
Verified the comment's justification is exact: `vm.hpp:457-458` gives `kSmallIntLo = -256`,
`kSmallIntHi = 256`, and a byte is 0..255 — strictly inside the permanently-rooted intern table.

**No sibling repeats the pattern with a wider value.** Exhaustive grep of every `make*` fed straight
into a callback arg array:
```
$ grep -rn -B6 "\.call(vm, args)" src/kirito/*.hpp | grep "makeInt\|makeString\|makeFloat\|vm.alloc"
src/kirito/bytes.hpp-294-       std::array<Handle, 1> args{vm.makeInt(c)};                       <- the documented one
src/kirito/stdlib_io.hpp-518-   std::array<Handle, 1> args{rs.add(vm.makeString(s))};           <- rooted
src/kirito/stdlib_matrix.hpp-244- std::array<Handle, 1> args{rs.add(vm.makeFloat(...))};        <- rooted (A16-1)
src/kirito/stdlib_tensor.hpp-1748- std::array<Handle, 1> args{rs.add(vm.makeFloat(...))};       <- rooted
src/kirito/runtime.hpp-1209-    std::array<Handle, 1> args{rs.add(vm.makeString(...))};         <- rooted
```
Every sibling roots via `rs.add`. `String.apply` (runtime.hpp:1201-1215) roots BOTH the per-char arg
and the callback result. Live-confirmed with an allocating callback over a 300-char string and a
300-byte Bytes under `--gc-threshold 1` (above). **Not a finding.**

---

## Differential test vs CPython (1124 generated cases)

The highest-yield probe I ran: generated 1124 method/argument combinations (`find`/`rfind`/`count`/
`startswith`/`endswith` × 10 window forms, `replace` × count, `split` × maxsplit, `strip` family,
`partition`, `zfill`, `ljust`/`rjust`/`center`, the 6 `is*` predicates, `removeprefix`/`removesuffix`,
`join`), ran them through `./build-debug/ki` and through CPython, and diffed.

**Result: 56 diffs / 1124 (95% exact agreement), collapsing into exactly 2 classes — both of which
turn out to be by design.** No crash, no memory error, no wrong value on any non-degenerate input.

### Class 2 (1 diff): `center` padding bias — BY DESIGN, do NOT "fix"
```
"ab".center(5, "*")   ki=*ab**    py=**ab*
```
Kirito always puts the extra pad on the RIGHT. CPython's `str.center` uses the quirky
`left = marg//2 + (marg & width & 1)` rule, which makes CPython's own `str.center` and
`format(s, "^")` **disagree with each other**. Kirito's rule is self-consistent (`format("ab","^5")`
and `"ab".center(5)` agree) and is **explicitly pinned in 4 tests**:
- `r8_types.ki:178` — "center puts the extra pad on the right for an odd gap"
- `r8_types.ki:179` — "center with a fill keeps the smaller half left"
- `labx_string_type.ki:214` — "center odd width (extra on right)"
- `deep_text.ki:136` — `"ab".center(5,"-") == "-ab--"`

Aligning to CPython would break all four. **Non-finding; flagging it here so the next round doesn't
re-litigate it.** Recommend it be added to `.audit/README.md`'s false-positives table.

### Class 1 (55 diffs): empty needle + degenerate window — see A07-1 below (LOW)

---

## FINDINGS

### A07-1: `Bytes._setstate_` re-homes an immutable, hashable value in place, corrupting any Dict/Set keyed on it — the guard DateTime already has is missing here and on BigInt  [severity: MED] [confidence: confirmed]

- **Location:** `src/kirito/bytes.hpp:306-311` (`Bytes._setstate_`), and the identical gap at
  `src/kirito/stdlib_int.hpp:674-681` (`BigInt._setstate_`).
  The **correct** implementation to copy is `src/kirito/stdlib_time.hpp:271-288` (`DateTime._setstate_`).

- **What:** `Bytes` is documented immutable (`bytes.hpp:22` "An immutable sequence of raw bytes") and
  is `hashable() -> true` (`bytes.hpp:69`), so it is a legal Dict key / Set element. But its
  `_setstate_` — reachable from ordinary Kirito, it is just an attribute — assigns
  `self_b(vm, self).data = ...`, mutating an established value **in place**. Mutating a live key
  changes its hash while it sits in a bucket, which breaks the Dict/Set invariant: the entry becomes
  unreachable, and a re-insert of the same logical key produces **a Dict containing two keys that
  compare equal**.

  This is not a novel hazard — the codebase already diagnosed and fixed it for `DateTime`, and the
  existing guard's comment states the contract that Bytes/BigInt violate verbatim
  (`stdlib_time.hpp:276-278`):
  > `DateTime` is immutable + hashable (a Dict/Set key). `_setstate_` exists ONLY for the
  > deserializer's `alloc(empty) -> _setstate_(epoch)` path; re-homing an established value in place
  > would corrupt any container keyed on it. So it is one-shot: refuse once built.

  That reasoning applies word-for-word to `Bytes` and `BigInt` — the three hashable serializable
  natives. Only `DateTime` got the guard. (The other `_setstate_` types — Matrix/Complex/Tensor/
  Random/parallel primitives — are all `hashable() -> false`, so they cannot key a container and are
  correctly unaffected.) This is exactly the "fix applied to one copy and not the other" DRY class the
  round brief calls out.

- **Repro** (`/tmp/a07_repro.ki`, real output; identical under `--gc-threshold 1`):
```kirito
var k = Bytes([1, 2])
var d = {}
d[k] = "payload"
discard k._setstate_("ZZZ")          # no guard: mutates the live key
d[Bytes("ZZZ")] = "second"
io.print("  len(d) =", len(d), "  keys =", List(d.keys()))
io.print("  -> a Dict with TWO equal keys:", List(d.keys())[0] == List(d.keys())[1])
```
```
$ ./build-debug/ki /tmp/a07_repro.ki
### Bytes: immutable + hashable, but _setstate_ re-homes it in place ###
  len(d) = 2   keys = [b'ZZZ', b'ZZZ']
  -> a Dict with TWO equal keys: True
### BigInt: same ###
  len(d2) = 2   keys = [42, 42]
  -> a Dict with TWO equal keys: True
### DateTime: ALREADY guarded (the correct behaviour) ###
  guarded: DateTime _setstate_: cannot re-initialize an established DateTime (it is immutable once built)
```
  Fuller corruption trace (`/tmp/a07_corrupt.ki`) — the entry goes write-only, exactly the shape the
  NaN-key false positive describes, but here with no IEEE-754 justification:
```
  k in d           = False    <-- key lost
  Bytes('ZZZ') in d= False
  len(d)           = 1   <-- still 1: an unreachable entry
  keys(d)          = [b'ZZZ']
  d.get(k, 'GONE') = GONE
```

- **Impact:** Any Kirito code that calls `_setstate_` on an established `Bytes`/`BigInt` silently
  corrupts every Dict/Set holding it. No crash, no error — `len()` and `keys()` disagree with `in`
  and `get`. Reachable from pure Kirito with no unsafe construct. Realistically hit by a hand-rolled
  or custom (de)serializer that reuses an existing object rather than a fresh one, and by anything
  that treats `_getstate_`/`_setstate_` as a public copy protocol. Not memory-unsafe (no UAF), which
  is why it is MED not HIGH.

- **Proposed fix:** Mirror DateTime's proven one-shot pattern in both types — no new concept, and it
  makes the three hashable natives agree:
  1. `BytesVal`: add `bool initialized_ = false;`; set it in the value ctor
     (`explicit BytesVal(std::string d) : data(std::move(d)) { initialized_ = true; }`);
     have `_setstate_` `throw KiritoError("Bytes _setstate_: cannot re-initialize an established Bytes (it is immutable once built)")`
     when `initialized_`, and set it on success.
  2. **Careful — a trap DateTime does not have:** the Kirito-visible no-arg `Bytes()` at
     `runtime.hpp:3012-3013` uses the SAME default ctor as the deserializer factory
     (`runtime.hpp:3024-3026`). A naive flag would leave `Bytes()` mutable-once. The no-arg
     constructor must construct an *initialized* empty value (e.g. `BytesVal(std::string())`),
     leaving the default ctor exclusively for the deserializer.
  3. `BigIntVal` (`stdlib_int.hpp:509-520`) has the identical shape (`BigIntVal() = default;` for the
     deserializer at `stdlib_int.hpp:694`, `explicit BigIntVal(Big)` for values, `hashable() -> true`)
     — apply the same flag + guard.
  - **Contract risk: none.** The deserializer path (`alloc(empty) -> _setstate_`) is untouched, since
    the factory-made object is uninitialized by construction. Verified the round-trip this must not
    break still works today (`/tmp/a07_ser.ki`: all 256 byte values, empty, 1000 NULs, 5000 high
    bytes, shared refs, through BOTH `dump` and `serialize`) — that script doubles as the
    must-still-pass check. Does not touch the documented `.audit/README.md` false positives.

- **Proposed test:** `tools/tests/scripts/spec_bytes.ki` (which already owns the Bytes protocol
  surface), named for the symptom:
```kirito
# _setstate_ is one-shot: re-homing an established Bytes would corrupt a Dict keyed on it.
assert throws(Function(): return Bytes([1,2])._setstate_("ZZZ")), "Bytes _setstate_ refuses an established value"
var k = Bytes([1, 2])
var d = {}
d[k] = "payload"
assert throws(Function(): return k._setstate_("ZZZ")), "a live Dict key cannot be re-homed in place"
assert k in d and len(d) == 1, "the Dict keyed on it is intact"
# the deserializer path must still work
assert dump.loads(dump.dumps(Bytes(List(range(0,256))))) == Bytes(List(range(0,256))), "dump round-trip still works"
```
  Mirror it for `BigInt` in `tools/tests/scripts/` alongside the existing `int`-module coverage.
  Both must be verified to FAIL on the unfixed build (they do — see the repro above).
