# A2 — EXECUTION / VM — Deep Audit Report (scan3, v1.17.1)

Scope: bytecode interpreter / stack VM, core value operations, int/float arithmetic, comparisons,
truthiness, control flow, try/catch/finally, exceptions, recursion guard, f-string caps, numeric
builtins. Files read in full: `value.hpp`, `bytecode_vm.hpp`, and the arithmetic/numeric/`Integer()`
sections of `runtime.hpp` (plus `vm.hpp` recursion guard, `stdlib_int.hpp` parser context).

Binary used: `build-bin/ki-asan` (cross-checked against `ki-release` / `ki-debug` where relevant).

## Result: 0 CONFIRMED user-code-provable bugs.

Every arithmetic edge, control-flow path, conversion, and comparison probed came out correct and
memory-clean under AddressSanitizer. One asan-only anomaly (deep recursion) was investigated and is a
sanitizer-frame-size artifact, NOT a shipped bug — detailed below as informational.

---

## Informational (NOT counted — sanitizer artifact, shipped binaries correct)

### `vm.hpp:293-308` (enterCall / recursion guard) · INFO · asan-only, release & debug correct

Reproducer:
```
var rec = Function(n):
    return rec(n + 1)
try:
    discard rec(0)
catch as e:
    io.print(String(e))
```
- `ki-asan`  → `AddressSanitizer: stack-overflow` (SIGSEGV, exit 141). Crash appears exactly at
  recursion depth 500 (the sanitizer preset's `maxCallDepth_`); depth ≤480 runs fine.
- `ki-release` → `maximum recursion depth exceeded` (catchable, exit 0).
- `ki-debug`   → `maximum recursion depth exceeded` (catchable, exit 0).

Cross-check that the guard genuinely bounds native stack even through deep C++ frames (native
higher-order call routed through `List.sort(key=...)` recursing) — `ki-release` still throws
`maximum recursion depth exceeded` cleanly, no crash.

Root cause: the guard already lowers its budget for sanitizer builds (`vm.hpp:584-592`:
`maxCallDepth_ = 500`, `maxStackBytes_ = 2 MiB` under `KIRITO_SANITIZER_BUILD`, vs 3000 / 6 MiB
otherwise). Under asan each `BytecodeVM::run` frame carries large redzones, so 500 frames exhaust the
main thread's 8 MiB stack at the exact count-limit boundary before the byte-budget check catches it.
The shipped (release/debug) binaries prove the guard works: unbounded recursion becomes a catchable
`KiritoError`, never a crash. This is not reachable as a defect in any shipped artifact, so per the
hard rule it is out of scope as a bug. It is only a *test-infrastructure* nuisance: the asan gate
itself can crash on any test that drives recursion to the limit. Optional hardening (not a bug fix):
drop the sanitizer `maxCallDepth_` further (e.g. 300) so the byte-budget, not the count, governs, and
the asan gate can exercise recursion-limit error paths without SIGSEGV.

### `runtime.hpp:507-520` (EqualsGuard) · INFO · cyclic `==` throws instead of terminating

```
var a = [1]; a.append(a)
var b = [1]; b.append(b)
io.print(a == b)   # -> error: maximum comparison recursion depth exceeded (cyclic structure?)
```
A self-referential structure compared with `==` raises a catchable, bounded error rather than looping
forever or crashing. This is a deliberate depth bound (fails loud and catchable), not UB. Noted only
because Python returns `True` here via identity short-circuit; Kirito's choice is defensible and safe.
Not a bug.

---

## Verified CORRECT (probed, not merely read)

All probes run on `ki-asan` unless noted; no asan/UBSan trace fired on any of them.

### int64 two's-complement wrap (documented, no UB) — `runtime.hpp:153-182,236-253,326`
```
9223372036854775807 + 1        -> -9223372036854775808
-9223372036854775808 - 1       ->  9223372036854775807
9223372036854775807 * 2        -> -2
-9223372036854775808 // -1     -> -9223372036854775808   (INT64_MIN/-1 wraps, no UB)
-9223372036854775808 % -1      ->  0
-(-9223372036854775808)        -> -9223372036854775808   (unary neg wraps)
2 ** 63                        -> -9223372036854775808
2 ** 64                        ->  0
0 ** 0                         ->  1
```
Overflow arithmetic done in `uint64_t` and reinterpreted — asan/UBSan clean.

### floor-div / floor-mod sign convention — `runtime.hpp:162-173,239-246,277-289`
```
7 // -3 -> -3     -7 // 3 -> -3
7 % -3  -> -2     -7 % 3  ->  2      (remainder takes divisor's sign)
7.0 % -3.0 -> -2.0   -7.0 % 3.0 -> 2.0
divmod(7,-3) -> [-3,-2]   divmod(-7,3) -> [-3,2]   (consistent floor semantics)
```

### div / mod / pow by zero — all raise catchable errors, none silently produce inf/NaN
```
1 // 0        -> "integer division by zero"
divmod(5,0)   -> "integer division by zero"
0 ** -2       -> "zero cannot be raised to a negative power"
0.0 ** -1.0   -> "zero cannot be raised to a negative power"
(-2.0) ** 0.5 (fractional neg base) -> raises (probed in prior rounds via numericBinary:295)
```

### int↔float EXACT comparison near 2^53 and 2^63 — `runtime.hpp:138-201,307-334`
```
9007199254740993 == 9007199254740992.0   -> False   (2^53+1 != float 2^53)
9007199254740993 >  9007199254740992.0   -> True
9223372036854775807 == 9223372036854775808.0 -> False (INT64_MAX != float 2^63)
9223372036854775807 <  9223372036854775808.0 -> True
1 == 1.0 -> True     1.5 == 1 -> False     0.0 == -0.0 -> True
```
No lossy `(double)int64` round-trip; `compareIntFloat` uses the shared 2^63 boundary constant.

### NaN / Inf / -0 — IEEE-correct
```
nan == nan -> False   nan < 1 -> False   nan > 1 -> False
inf > INT64_MAX -> True   -inf < INT64_MIN -> True
inf % 2.0 -> nan   5.0 % inf -> 5.0   -5.0 % inf -> inf   1e17 % 3 -> 1.0  (fmod-based, exact)
```

### hash / equals agreement across Integer and Float — `runtime.hpp:336-343`
```
{1,2,3}.contains(1.0) -> True
d[1]="int"; d[1.0]="float"; d[1] -> "float", len 1   (equal keys unify)
d2[9007199254740993]="a"; d2[9007199254740992.0]="b"; len 2   (distinct: exact)
```

### Integer(String) parsing / overflow — `runtime.hpp:3574-3643`
```
Integer("9223372036854775807") ->  max        Integer("-9223372036854775808") -> min
Integer("9223372036854775808") -> throws (decimal > INT64_MAX, no silent wrap)
Integer("18446744073709551616") -> throws (>= 2^64)
Integer("0xFFFFFFFFFFFFFFFF") -> -1   Integer("-0x8000000000000000") -> INT64_MIN
Integer("0x10000000000000000") -> throws (> 2^64)
Integer("0b111") -> 7   Integer("0o777") -> 511   Integer("0X1f") -> 31
Integer("  42  ") -> 42 (surrounding ws ok)   Integer("007") -> 7   Integer("+5") -> 5
Rejected (no silent junk): "1_000", "1e3", "12abc", "", "  ", "0ximportant", "0x0x5"
```
Decimal magnitudes fail-fast on overflow; hex/oct/bin are bit patterns and wrap intentionally per docs.

### try / catch / finally control flow — `bytecode_vm.hpp:452-501,533-578`
```
return inside try  -> finally runs, then returns "try"
finally return      -> overrides try's return ("finally")
break crossing finally    -> finally body runs for the broken iteration, loop exits
continue crossing finally -> finally runs each iter, skipped element omitted ([1,3])
throw + finally return    -> exception swallowed, returns "finally"
```

### exceptions — object identity preserved, typed catch, builtin errors as String
```
throw MyErr(42); catch as e -> type(e)=="MyErr", e.code==42
catch MyErr as e -> typed match binds the object
1 // 0; catch as e -> type(e)=="String", e=="integer division by zero"
nested throw/re-throw preserves the original instance (rethrown code 99)
```

### recursion guard bounds native stack (shipped binaries) — `vm.hpp:293-308`
`ki-release`/`ki-debug`: unbounded `rec(n+1)` and recursion through a native higher-order
(`sort(key=...)`) both throw `maximum recursion depth exceeded` — catchable, no crash.

### f-string / format-spec caps — `bytecode_vm.hpp:364-373`, `runtime.hpp:~3017`
```
f"{x:10000000000}"     -> throws (width cap; no OOM)
f"{x:.10000000000f}"   -> throws (precision cap)
f"{5:100}" == " "*99+"5"  -> True     len(f"{3.14159:.50f}") -> 52   (normal specs work)
```

### round (half-away-from-zero, per docs 08-builtins.md:132) & huge ndigits
```
round(0.5)=1 round(1.5)=2 round(2.5)=3 round(-0.5)=-1 round(-1.5)=-2 round(2.675,2)=2.67
round(2.675, 100000000000) -> unchanged (no overflow crash; precision guard holds)
```

### truthiness — `Bool(0)/Bool(0.0)/Bool("")/Bool([])/Bool({})/Bool(-0.0)` all False; `Bool(nan)` True.

### short-circuit & ordering errors
```
True or side()  -> side() not called (len(called)==0)
False and side()-> side() not called
1 < "a"         -> "unsupported operand type 'String' for comparison with 'Integer'" (structured)
chained `1 < 2 < 3` -> compile error (disallowed by design; use `and`)
```

---

## Notes for the orchestrator
- No HIGH/MED findings to re-verify.
- The recursion asan crash is the only thing that *looks* alarming; I confirmed it is asan-frame-size
  hitting the count limit before the byte budget, and that release/debug bound it correctly. Treat as
  test-infra tuning, not a language/VM bug. Suggested (optional) one-line tweak: lower the
  `KIRITO_SANITIZER_BUILD` `maxCallDepth_` (vm.hpp:585) below ~350 so the asan gate can run
  recursion-limit tests without SIGSEGV.
