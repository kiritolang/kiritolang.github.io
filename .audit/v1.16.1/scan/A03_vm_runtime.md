# A03 — Bytecode VM + runtime (operators / calls / members / exceptions / control flow)

Audit round v1.16.1. Scope: bytecode_vm.hpp, runtime.hpp, control.hpp, exceptions.hpp,
function.hpp, and VM-relevant invariants of bytecode.hpp/compiler.hpp.

## Findings

### F03-1 [Med] Float `%` / `//` lose precision for large operands; `%` can return a value == the divisor (out of the documented `[0,y)` range)
- runtime.hpp:281-284 (`numericBinary`, Float Mod branch) computes `x - std::floor(x/y)*y`; the FloorDiv branch (277-280) computes `std::floor(x/y)`. For large `|x/y|` the product `floor(x/y)*y` rounds back to `x`, so the remainder collapses to `0.0` (or, near a boundary, to exactly `y`).
- Trigger/repro:
  - `1e17 % 3.0` → Kirito `0.0`; correct (C `fmod`, Python, and Kirito's OWN `math.fmod(1e17,3.0)`) → `1.0`. Same for `divmod(1e17,3.0)` remainder.
  - `(-1e-16) % 1.0` → Kirito `1.0` — this equals the divisor, violating the documented `%` result range `[0, y)` (docs/pages/09-types.md:63). Python → `0.9999999999999999` (`< 1.0`).
- Verified-real: CONFIRMED against the running debug `ki` and cross-checked with Python + `math.fmod`. Internal inconsistency: the `%` operator disagrees with the module's own `math.fmod`.
- Fix idea: base float `%`/`//` on `std::fmod` with a sign/one-ulp correction (Python's `float_rem`/`float_floor_div` algorithm): `r = fmod(x,y); if (r && (r<0)!=(y<0)) r += y;` for `%`, and derive `//` as `(x - r)/y` (or `floor` with an fmod-corrected residual). Add tests: `1e17 % 3.0 == 1.0`, `(-1e-16) % 1.0 < 1.0`, and `(x//y)*y + x%y == x` for large x.

## Coverage notes

Scope covered by deep read + live probes against `build-debug/ki` (read-only; no build run).
The VM/runtime is very hardened (5 prior rounds + asan/tsan). Everything below was CONFIRMED
working unless flagged as a gap.

### Exceptions / control flow — VERIFIED working
- try/finally with `return` in the finally overriding an in-flight exception → returns the finally value.
- Nested try/catch INSIDE a finally: reraise reports the OUTER exception's true origin line (E1 at its
  throw site), not the inner already-handled one — the `$exc`/`excSpan` parking (A04) holds. Confirmed
  both the value AND the line survive.
- `continue`/`break` inside a finally in a loop; `return` crossing 1–2 `with` blocks runs every `_exit_`
  in reverse order; break/continue crossing a `with` runs `_exit_`.
- `throw` inside a `catch` propagates to the outer handler; re-throw with a rebuilt message works.
- Bare `catch:` and `catch as e:` catch a native `std::exception` crossing the boundary (surfaced as a
  String, e.g. `sqrt: math domain error`); typed `catch String/Integer/List as e` and user-class
  `catch Base as e` (subclass match via the class chain) all route correctly.
- Strict PEP-479 StopIteration attribution: a `throw StopIteration()` at `_next_`'s own frame ends the
  loop; one leaking from a deeper call surfaces as an uncaught error (depth-tagged). Confirmed.
- Recursion guard (count + native-stack-bytes) throws a CATCHABLE "maximum recursion depth exceeded"
  for direct, mutual, and native-higher-order (`sorted(key=…)`) recursion — no SIGSEGV.

### Calls / kwargs — VERIFIED working
- positional-after-keyword → deferred CATCHABLE runtime throw; duplicate kwarg → "multiple values";
  unknown kwarg → "unexpected keyword argument" (both KiFunction and signatured natives via bindArgs).
- Defaults evaluated at call time, once per call (mutable `xs=[]` is fresh each call); a default may
  reference an EARLIER param or an enclosing name; a LATER-param reference is a compile-time name error.
- Multi-level `self._super_()` chain (C→B→A) composes; `_super_()(x)`/`[i]` operator syntax on the view
  throws "type 'Super' is not callable/indexable" while the named `._call_(x)` form works (A09 rule).
- `_call_`/instance kwargs; bound-method kwargs; class instantiation forwards kwargs to `_init_`.

### Operators — VERIFIED working
- Exact IEEE-754 `==` (`0.1+0.2 != 0.3`, large int↔float exact, `0.0==-0.0`, `NaN!=NaN`); reflected
  `==`/`!=` when only the RIGHT operand is an instance with `_eq_`; arithmetic does NOT reflect onto a
  right instance (documented). Integer wraparound (`-INT64_MIN`, `INT64_MIN//-1`, `%-1`) all defined.
- Integer floor `%`/`//` sign semantics match Python exactly. `3 * "ab"` reflected repetition. Set/Dict/
  List equality honoring user `_eq_`/`_hash_` (`{P(1),P(2)}=={P(2),P(1)}`, `P(1) in dict`). Short-circuit
  `and`/`or`/`not`, conditional expr short-circuit. Numeric fast path agrees with the generic path.
- Kirito has NO Python-style comparison chaining (`1<2<3` parses left-assoc → `True<3` → type error) —
  by design, not claimed.

### Control structures — VERIFIED working
- switch: no fallthrough; duplicate `default` and empty case body are parse errors; duplicate case value
  is a catchable runtime throw when reached; non-scalar/`1.0`-vs-`case 1` subjects fall to `default`
  (fast O(1) hash-dispatch key matches `scalarSwitchKey`).
- Unpacking: starred surplus (`a,*b,c`), too-few count error, nested; `for k,v in …`; iterate/unpack of
  None → clean "not iterable".

### Gaps / tests to add
- **Float `%`/`//` for large or boundary operands** (F03-1) — no test exercises `1e17 % 3.0`,
  `(-1e-16) % 1.0`, or `divmod(1e17, 3.0)`; add them (and the `(x//y)*y + x%y == x` identity for large x)
  once the fmod-based fix lands. This is the one confirmed correctness bug.
- No behavioral test found asserting the `%` result-range invariant `0 <= a%b < b` (positive b) — worth a
  property-style test given F03-1.
- `_exit_`/`_enter_` missing on a `with` context throws only when the missing method is reached (after the
  body for `_exit_`); acceptable, but there is no negative test asserting the diagnostic.
