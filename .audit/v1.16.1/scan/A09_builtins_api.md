# A09 — free builtins + value.hpp C++ embedding API

Scope: `value.hpp`, `builtins.hpp`, free builtins in `runtime.hpp` (installBuiltins).
Method: read for correctness + adversarial runs against `./build-debug/ki`. Each finding marked CONFIRMED / assessed.

Overall: this surface is unusually well-hardened (the A19-1/A19-2 GC-rooting fixes are thorough, the type
converters reject the obvious garbage, and value.hpp has a real C++ unit-test corpus:
`test_value*.cpp`, `test_pinned_handle.cpp`, `test_r4/r7/r8_*api.cpp`, `test_cppref_deep.cpp`). Findings
below are mostly Low / latent / coverage notes; no High confirmed.

---

### F09-1 [Low] Integer() decimal string overflow-throw boundary is 2^64, not 2^63 — silent wrap in (2^63, 2^64]
- runtime.hpp:3444-3449 — the String→Integer path parses the magnitude with `std::stoull` (base 10) and
  bit-casts. So `Integer("9223372036854775808")` (2^63) → INT64_MIN, `Integer("18446744073709551615")`
  (2^64-1) → -1, but `Integer("99999999999999999999")` (>2^64) THROWS "cannot convert String to Integer".
- Why: the throw only fires when stoull itself overflows (magnitude > 2^64). A *decimal* magnitude in
  (2^63, 2^64] silently reinterprets as a negative two's-complement value instead of throwing an
  out-of-range error, so the "overflow throws" contract has an asymmetric boundary.
- Trigger: `Integer("9223372036854775808")` → `-9223372036854775808` (confirmed via ki).
- Assessment: DELIBERATE and consistent with the lexer's `intLiteral` (the comment at :3441 says
  "mirroring the lexer's intLiteral, so the full 64-bit range round-trips" — needed for `Integer(hex(-1))`
  and `Integer(String(INT64_MIN))`). A hex/oct/bin magnitude legitimately spans the full unsigned range;
  applying the same rule to a *decimal* string is the surprising part (Python yields a bigint; the task's
  own note lists `Integer("99999999999999999999")` as "overflow throw"). Not a bug per the current design,
  but the boundary is worth a one-line doc note or a decimal-only >INT64_MAX guard.
- Verified-real: CONFIRMED behavior; classified as design/doc gap, Low.

### F09-2 [Low] Float("1e400") throws, but Float("inf") returns inf — inconsistent overflow handling
- runtime.hpp:3478 + common.hpp:84-85 — `parseDouble` throws `std::out_of_range` when strtod overflows to
  ±HUGE_VAL, so `Float("1e400")` → "cannot convert String to Float: '1e400'", while `Float("inf")` /
  `Float("-inf")` / `Float("nan")` all succeed (confirmed via ki).
- Why: a decimal that overflows to infinity is rejected, but the literal token `inf` is accepted — two
  ways to name +inf, one throws.
- Assessment: DELIBERATE and consistent with the lexer (a source literal `1e400` is likewise a parse
  error via parseDouble), and defensible (overflow is usually a mistake). Python returns inf for both;
  Kirito diverges intentionally. Low / documentation nuance.
- Verified-real: CONFIRMED behavior; Low.

### F09-3 [Low] round(x, nd) MSVC/Windows path can truncate for huge x AND huge nd (silent wrong result)
- runtime.hpp:3595-3598 — on a platform where `long double == double` (MSVC), the `#else` branch does
  `char buf[512]; std::snprintf(buf, 512, "%.*f", (int)nd, x); strtod(buf, ...)`. `nd` is only bounded by
  `nd <= 323` (the `nd > 323` early-return is at :3581), and `x` may be up to ~1.8e308. For e.g.
  `round(1e300, 323)` the formatted text is ~301 integer digits + '.' + 323 fractional ≈ 625 chars > 511,
  so snprintf truncates and strtod parses a truncated (wrong) number.
- Trigger: `round(1e300, 323)` on an MSVC build (Windows). NOT reachable on this Linux dev box — the
  `LDBL_MANT_DIG > DBL_MANT_DIG` long-double branch (:3586-3588, no fixed buffer) is taken here, so I
  cannot repro at runtime.
- Fix: size the buffer to the worst case (DBL_MAX_10_EXP + nd + margin, ~700), or `std::to_string`-style
  dynamic buffer, on the MSVC path.
- Verified-real: CONFIRMED by reading; platform-specific + extreme inputs → Low.

### F09-4 [Low] Value::at(ptrdiff_t) passes an unrooted key handle into getItem
- value.hpp:1020-1024 — `Handle key = vm_->makeInt(i); std::array<Handle,1> keys{key};
  return Value::adopting(*vm_, ref().getItem(*vm_, keys));`. The *result* is pinned (adopting), but `key`
  itself is not rooted. For an index outside the small-int intern range (`|i| > 256`) `makeInt` allocates a
  fresh IntVal that lives only in the local `keys` array; if any `getItem` implementation allocated before
  reading the key, a GC could sweep it.
- Assessment: SAFE in practice — every current getItem (List/Dict/String/Bytes/Tensor/…) consumes the key
  value before it allocates its result, and small indices hit an interned int. But it is the one spot in
  value.hpp that hands an unrooted fresh allocation into the protocol, inconsistent with the file's own
  defensive-rooting discipline (cf. the RootScope-everywhere pattern in Dict::set). A one-line
  `RootScope rs(*vm_); rs.add(key);` would make it match.
- Verified-real: CONFIRMED as a latent gap (not a live bug); Low.

### F09-5 [Low] Dict / Set range-for iterators snapshot the whole container TWICE (O(n) copy in end())
- value.hpp:795-800 (Dict) and 867-872 (Set) — `begin()` calls `raw().pairs()` / `raw().items()` building
  a full vector copy, and `end()` builds the SAME full vector copy again only to read `.size()`. A
  `for (auto [k,v] : dict)` therefore materializes two independent O(n) Handle-vector copies (each iterator
  owns its own `ps_`/`hs_`), then compares only the index `i_`.
- Why: functionally correct (index-only `operator!=`), but 2× the allocation/copy of a single snapshot.
  `end()` needs only the count, not a second copy.
- Fix: have `end()` return `Iterator(vm_, {}, size())` (empty vector, index=size) — `operator*` is never
  called on end, and `operator!=` only inspects `i_`.
- Verified-real: CONFIRMED (efficiency, not correctness); Low.

### F09-6 [Low/Coverage] value.hpp paths with no evident C++ unit coverage
- Assessed by reading (these are C++-only, unreachable from .ki):
  - `PinnedHandle` self-copy-assign / self-move-assign — the code is correct (pin-before-reset at
    value.hpp:942-950, `this != &o` guards), but a targeted self-assign test would lock it in.
    `test_pinned_handle.cpp` exists; confirm it exercises self-assignment and the copy-refcount balance.
  - The `Anything` variant coverage of every C++ integral width (`unsigned long long`, `long`, `float`)
    feeding `List(vm,{...})` / `Dict(vm,{{...}}))` initializer lists.
  - `Value::items()` pinning of String/Bytes per-element boxes (value.hpp:237-248, the A07-4 fix) — a test
    that iterates a String via the C++ `items()` and then forces a GC before use would guard the pin.
  - `Args::require` / `Args::at` arity-error wording.
- Not defects; listed so the round can add focused C++ tests where the GC-safety comments claim invariants
  that only a sanitizer run would catch if they regressed.
- Verified-real: coverage observation.

### F09-7 [Low] Value::equals() doc-comment overstates its equivalence to Kirito `==`
- value.hpp:250-253 — the comment calls `equals()` "the same predicate Kirito's `==` uses". `equals()`
  calls the raw object-protocol `Object::equals` slot, whereas Kirito source `==` goes through
  `applyBinaryOp(BinOp::Eq)` (which is also what `Value::operator==` uses). For InstanceValue these
  largely converge (InstanceValue::equals dispatches to a user `_eq_`, class_value.hpp:129), but they are
  not literally the same path, and for a native type that overrides the operator dispatch but not the
  structural slot they could diverge. Minor doc imprecision — clarify that `equals()` is the structural
  slot and `operator==` is the full operator dispatch.
- Verified-real: CONFIRMED doc nuance; Low.

---

## Adversarial runs that PASSED (no defect) — recorded for the round
- `Integer("0xFF")`=255, `Integer("  10  ")`=10, `Integer(3.9)`=3 (truncates toward zero),
  `Integer(True)`=1, `Integer("0b1010")`/`"0o17"` ok, `Integer(hex(-1))`=-1, `Integer(" +0x1F ")`=31,
  `Integer(">2^64")` throws, doubled-prefix `0x0x5` / `--5` / `+ 5` rejected (runtime.hpp:3424-3438).
- `Float("nan")`/`"inf"`/`"-inf"`/`"  3.5 "` ok; `Float("1_000")` rejected; C99 hex-float rejected (:3475).
- `Bool([])`=False; `List("ab")`=['a','b']; `Set([1,1])`={1}; `Dict([["a",1],["b",2]])` ok;
  `List(dict)`/`Set(dict)` iterate keys; `Bytes([256])` and `Bytes([-1])` both throw "out of range (0..255)".
- `round`: half-away-from-zero (round(2.5)=3, round(-0.5)=-1), `round(2.675,2)`=2.67 (long-double
  descaling, :3586), negative nd ok, huge ±nd handled (:3581-3582).
- `pow(2,10,1000)`=24, `pow(2,10,1)`=0; mod 0 / negative mod / negative exp all throw (:3966-3971);
  int128 reduction avoids the ~2^62 modulus overflow (:3972-3982).
- `divmod` floor semantics; `bitand(-1,255)`=255; `shl(1,63)`=INT64_MIN; `shr(-8,1)`=-4 (arithmetic);
  shift ≥64 and negative shift handled (:3943-3951); `bin(-5)`="-0b101"; `chr(0)`/`chr(0x10FFFF)` ok,
  surrogate + out-of-range chr throw (:3897-3899); `ord` of multi-cp string throws.
- `isinstance`/`hasattr`: None attr counts as present, class value / plain function → False, built-in type
  constructor and type-name-String args both work; `abs(INT64_MIN)` documented wrap.
- value.hpp GC rooting: fresh-alloc ctors (Value/Integer/Float/String/Bytes/List/Dict/Set) all `adopt()`
  (pin); List/Dict/Set initializer-list + bulk ctors root each element as materialized (the A19-1 fix,
  value.hpp:560-574/658-674/821-826); pop/call/getAttr/at/operators all pin their fresh result
  (`adopting`); operator ==/</… use `truthy()` not a BoolVal cast (safe against a non-Bool user dunder).
  PinnedHandle copy/move/self-assign refcounting is balanced (vm.hpp:131-136 refcount map).
