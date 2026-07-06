# Kirito v1.14 — consolidated findings (triaged)

All 19 subsystem agents landed (A01–A19) + clang-tidy static analysis (`static/TRIAGE.md`,
correctness-clean). This file dedups, ranks, and marks each finding's fix disposition. Detail lives
in `agents/AXX_*.md`.

Legend: **FIX** = confirmed bug, will fix + test this round · **DECIDE** = design decision ·
**DOC** = documentation-only · **DEFER/OPEN** = carried from v1.13, re-confirmed, low value ·
**NON-BUG** = verified intentional.

---

## HIGH — confirmed memory-safety crash

### F-1 (A08-1) — Set/Dict.clear() + Set.pop() bypass the `probing_` reentrancy guard → heap corruption  **[FIX]**
- location: `src/kirito/collections.hpp` (SetVal/DictVal `clear`/`pop`; also `SetVal::equals`)
- `add`/`set`/`remove`/`discard` all check `if (probing_) throw`, but `clear`/`pop` don't. A user
  `_hash_`/`_eq_` that calls `container.clear()` while a C++ probe loop holds a reference to the
  bucket `std::vector<Handle>` → the vector is destroyed under the loop → **double-free / heap
  corruption**. Reproduced as a hard `free(): double free` abort on plain `build-debug/ki` (no ASan)
  via `K(5) in s`, `d.get(K(5))`, `set1 == set2`, and `s.add(K(5))`.
- Also: `SetVal::equals` iterates live (kiEquals has no Set==Set fast path) → needs a snapshot/ProbeScope.
- fix: add `if (probing_) throw` to `clear`/`pop`; wrap `SetVal::equals` in the probe guard/snapshot.
- test: `.ki` — a class whose `_hash_`/`_eq_` mutates the container mid-op must throw cleanly (C++ + .ki).

---

## MEDIUM — confirmed correctness / memory / security / DoS

### F-2 (A06-1) — `InstanceValue::str` cycle guard checks `ctx.active` but never `ctx.depth` → unbounded native recursion  **[FIX]**
- location: `src/kirito/runtime.hpp:1913` (InstanceValue::str)
- A `_str_` that returns *another* instance recurses in native C++ with no depth cap → segfault
  (exit 139) at ~200k chain depth. Analogue of the already-fixed `_iter_`-self crash. Reachable with
  one ordinary class.
- fix: thread `ctx.depth` through the str path with the standard depth guard (mirror the `_iter_` fix).
- test: `.ki` deep `_str_`-returning-instance chain must throw "maximum ... recursion depth", not crash.

### F-3 (A09-1) — `format(x)` with empty spec is lossy for floats  **[FIX]**
- location: format-mini-spec builtin (`runtime.hpp`, `applyFormatSpec`)
- Empty spec routes through `applyFormatSpec` (default `'g'`/prec-6) so `format(2.0)`→`"2"`,
  `format(1234567.0)`→`"1.23457e+06"`, while `f"{x}"`/`"{}".format(x)`/`String(x)` all use
  `stringify`→`"2.0"`/`"1234567.0"`. The in-code comment claims "No spec == String()" — code breaks it.
- fix: short-circuit an empty spec to `stringify` (as the f-string path already does).
- test: `.ki` — `format(2.0)=="2.0"`, `format(1234567.0)` round-trips; float round-trip pins it.

### F-4 (A11-1) — embedded NUL byte silently truncates every path (validation/sandbox bypass)  **[FIX]**
- location: `src/kirito/stdlib_io.hpp` / `stdlib_path.hpp` path entry points
- `io.open("real\x00.blocked","w")` writes `real`; `path.exists` truncates too. Python raises
  `ValueError: embedded null byte`. Security-class: a NUL defeats extension/suffix validation.
- fix: reject an embedded NUL in a path argument (shared helper) with a clear error.
- test: `.ki` — open/exists/isfile with a NUL-containing path must throw.

### F-5 (A11-19) — `BytesIO.truncate()` after a large seek bypasses the 256 MiB write guard  **[FIX]**
- location: `src/kirito/stdlib_io.hpp` BytesIO
- `b.seek(400000000); b.truncate()` materializes a 400 MB buffer unguarded (truncate-that-extends).
- fix: apply the same size guard to `truncate`'s resize path.
- test: `.ki` — truncate past the guard must throw.

### F-6 (A13-1) — tensor `max`/`min`/`argmax`/`argmin` are order-dependent under NaN  **[FIX]**
- location: `src/kirito/tensor.hpp` reductions
- `max([nan,1,3])→nan` but `max([1,nan,3])→3.0` (NaN skipped unless it's the seed). Same multiset →
  different per-axis answer. Diverges from `sum`/`mean` (propagate NaN) and from NumPy (propagates).
- fix: make the reduction NaN-propagating (if any element is NaN, result is NaN; argmax/argmin
  consistent) — matches NumPy `max`/`amax` and Kirito's own `sum`/`mean`.
- test: `.ki` — order-independence for both element orders; per-axis consistency.

### F-7 (A14-1) — serde `_getstate_` returning a Dict/Set keyed by a content-hashed member breaks `_setstate_` read-through  **[FIX]**
- location: `src/kirito/stdlib_serde.hpp` (pass ordering; comment at :216-218)
- `_setstate_` runs in pass 3, but a state Dict/Set keyed by a content-hashed member (Bytes/instance/
  Matrix) is only wired in pass 4 → `_setstate_` reading through the state sees it EMPTY. Confirmed:
  `{Bytes([9]):42}` state sums to 0 not 42, on both `serialize` and `dump`. Silent data corruption;
  contradicts the in-code contract comment. Adjacent to the v1.13 A17-1 fix.
- fix: ensure a container passed to `_setstate_` is fully wired before `_setstate_` runs (order the
  content-hashed wiring before the `_setstate_` pass for state containers, or wire on demand).
  CAREFUL — the v1.13 A17-1 fix deferred these to avoid a different regression; must not reintroduce it.
- test: `.ki` — a class with `{Bytes:val}` state + `_setstate_` reading it round-trips through both codecs.

### F-8 (A14-2) — json `stringify` indent path: signed-overflow UB + raw allocator message  **[FIX]**
- location: `src/kirito/stdlib_json.hpp:320` `(depth+1)*indent`
- UBSan-latent signed overflow at huge indent + deep nesting; a huge indent surfaces a raw
  `basic_string::_M_create` instead of a clean message.
- fix: clamp/validate `indent` at entry (small sane cap, e.g. ≤ a few hundred) with a clear error.
- test: `.ki` — `json.dumps(x, indent=<huge>)` throws cleanly.

### F-9 (A15-1) — `Socket.listen()` reads `sock.fd` directly instead of `fdOrThrow` (one-line)  **[FIX]**
- location: `src/kirito/stdlib_net.hpp:1059`
- listen-after-close leaks raw `"Bad file descriptor"` instead of the contractual
  `"listen: socket is closed"`. Violates CLAUDE.md + the `verify_net.ki:284` comment.
- fix: route `listen` through `fdOrThrow` like every other method.
- test: `.ki` — `s.close(); s.listen()` must throw "socket is closed".

### F-10 (A19-1 + A19-2) — `Value` operators / `call`/`getAttr`/`at`/`List::pop` return an unpinned fresh handle → GC-before-use dangles  **[FIX]**
- location: `src/kirito/value.hpp` (Value arithmetic/unary operators; call/getAttr/at; List::pop)
- The `applyBinaryOp`/call/get result is a fresh unrooted allocation wrapped in the non-pinning
  `Value(vm, Handle)` ctor. `String::operator+` DOES pin (safe) but `Value a+b` doesn't; `List::pop`
  orphans the element then returns it unpinned. A GC between the op and first use sweeps it → dangling.
- fix: pin the returned handle (use the pinning wrapper ctor / `pinHandle`) as `String::operator+` does.
- test: C++ — force GC (`setGcThreshold(1)`) between an operator/pop result and its use; assert survival.

### F-11 (A17-1) — regex capture-group blowup still live (untrusted-pattern DoS)  **[FIX]**
- location: `src/kirito/regex_engine.hpp`
- `(a?)`×500 over 10k input = 20.8s; `(.)`×200 over 20k = 3.3s. The v1.13 numGroups≤1000 cap is far
  too high and there is no runtime step/time budget. Linear-time matching is a stated security property.
- fix: add a match-step budget in `run()` (throw on exceeding it) and/or lower the group cap.
- test: `.ki` — a pathological pattern over a large input throws a budget error instead of hanging.

### F-12 (A16-1) — `time.sleep(inf)` / `sleep(1e19)` → float-cast-overflow UB  **[FIX]**
- location: `src/kirito/stdlib_time.hpp` sleep
- `sleep(inf)`/`sleep(1e19)` return immediately via int64 `duration_cast` UB; `sleep(1e18)` hangs.
  The one float entry point missing the finite/range guard the rest of the numeric stack applies.
- fix: reject non-finite + clamp/reject out-of-range before `sleep_for`.
- test: `.ki` — `sleep(inf)`/`sleep(nan)`/`sleep(1e30)` throw.

---

## LOW — confirmed, small; fix the cheap correctness ones

### F-13 (A04-1) — `_not_` return value not enforced to be a Bool (unlike `_bool_`)  **[FIX]**
- `not instance` can return a String, breaking the "logical `not` yields a Bool" contract. `_bool_`
  throws on non-Bool; `_not_` should too. Cheap, consistent. test: `.ki` errors case.

### F-14 (A04-2) — cyclic-structure guard says "maximum **equality** recursion depth" for `<`/sort/min/max  **[FIX]**
- Wrong word for an ordering op. Trivial message fix (make it neutral or op-aware). test: `.experr`.

### F-15 (A10-2) — `hasattr` probe catches only `KiritoError`; a native `getAttr` throwing `std::exception` escapes  **[FIX]**
- Widen the catch to `std::exception` (as the language does at the try/catch boundary) so `hasattr`
  returns False instead of propagating. test: needs a native throwing getAttr — C++ or a stdlib case.

### F-16 (A13-2) — `complex.atan(±i)` returns `0.0+infi` instead of throwing a domain error  **[FIX]**
- Its mirror `atanh(±1)` is guarded; make `atan(±i)` consistent. test: `.ki` raises.

### F-17 (A13-3) — `Matrix._setstate_([-1,0,[]])` builds a garbage matrix (rows()==-1)  **[FIX]**
- The complex sibling already guards negative dims; add the same guard to real Matrix. test: `.ki` raises.

### F-18 (A12-1) — NaN defeats `gauss`/`expovariate`/`uniform` param validation → quiet NaN  **[DECIDE→FIX]**
- `gauss(0, nan)` returns quiet NaN vs the strict `sigma<0` throw beside it. Consistent policy =
  reject non-finite params. Aligns with the math-module domain-throw policy. test: `.ki` raises.

### F-19 (A17-2) — `regex.sub` with negative `count` replaces ALL; Python replaces NONE  **[FIX]**
- Match Python: negative count = no replacement (or document). Cheap. test: `.ki`.

### F-20 (A13-4) — `Complex` has value equality but no `hash` → unhashable  **[DECIDE]**
- Can't key a Set/Dict despite `==`. Adding `_hash_` consistent with equality is correct but touches
  the type's contract; low priority. Likely FIX (hash by (re,im) bits) — mirrors DateTime.

---

## DECIDE — design decisions (likely DOC or NON-BUG)

- **A02-1** (medium-labeled): reading a local before its `var` resolves to an enclosing/module binding
  (no UnboundLocalError). This is the **documented** membership-scoping model ("resolution is by scope
  membership") — Kirito deliberately differs from Python here. Disposition: **DOC** (note the footgun
  in the language guide) unless we choose to add a use-before-decl diagnostic. Not a silent-wrong-value
  in the byte-for-byte sense (slot and name paths agree). → lean **DOC/NON-BUG**.
- **A03(v14)-1**: `switch` case labels accept arbitrary runtime exprs (`case g()`, `case 1+1`) vs the
  documented "constant scalars". Either tighten the compiler (reject non-constant labels) or relax the
  docs. → **DECIDE**; leaning tighten (a runtime-expr label is almost certainly a user error) — but
  needs care not to break `case -1` (already handled via constSwitchKey). Revisit; may **DOC** for now.
- **A02-2** (low): unused-result false positive on `cond and f()` / `f() if c else g()` when f/g return
  None. Analyzer is right for a bare call, over-eager once wrapped. → **FIX if cheap**, else DOC.
- **A09-1 sibling A09-2/3/4, A11 lows, A16-3/4/5, A18-1..4**: cosmetic / documented simplifications →
  **DOC or skip**.

---

## OPEN — carried from v1.13, re-confirmed, evaluate for this round

- **A04-3 (v1.13 A05-2)**: `excSpan` clobbered by a nested `try` inside a `finally` → wrong error
  location. Correctness (diagnostic quality). Medium-ish. **Evaluate FIX.**
- **A04-4 (v1.13 A06-1)**: `!=` asymmetric for a class defining only `_ne_` on the RHS. Correctness.
  **Evaluate FIX.**
- **A19-5 (sys)**: `sys.exit` doesn't flush open `fstream` files → file left empty. Confirmed live.
  Real data-loss bug. **Evaluate FIX** (flush registered streams on exit).
- **A19-2/A19-3/A19-9 (proc)**: POSIX pipe fd leak on error; Windows timeout UB; neg/NaN timeout
  infinite wait. Platform/robustness. **Evaluate FIX** (neg/NaN timeout is cheap + testable on Linux).
- **A08-3/4/5/6 (v1.13 A09-3/4/5/6)**: unhashable-`in` dict-throws-vs-set-False asymmetry; empty-bucket
  leak; subset-family unrooted; phantom `ValueKind::Array`. Mixed. **Evaluate** subset-unrooted (GC) first.
- **A07-1 (medium latent UB)**: `Object::operator new` lacks `align_val_t` overload → a future
  over-aligned Object subclass silently under-aligned by the pool (release-only, asan-invisible). No
  current trigger. **FIX (defensive)** — add aligned new/delete overloads + optional static_assert.

---

## Coverage gaps (fold into the fix tests where cheap; else note)

A01 (trailing-comma, BOM, 1.e5, unexpected-indent), A03 (const-dedup distinctness, switch NaN),
A07 (arena capacity stability, pool internals — release-only), A08-2 (List NaN sort/min/max untested),
A10 (round/abs on Complex), A12 (Random `_setstate_` error paths, C++ 7/11 methods), A13 (NaN
reduction untested, reshape -1), A14 (content-hashed `_setstate_`, huge json indent), A16 (sleep
guard, DateTime.format empty), A17 (external/dynamic-Huffman fixture, decompression-bomb assertion),
A19 (GC-between-op-and-use — the F-10 vector).

## DRY / single-source-of-truth

A01-N7/N8 (parseFString hand-rolled quote/escape scanners; parseInlineStatement vs parseStatement),
A03(v14)-4 (switch-key logic triplicated compiler/runtime), A10-4 (base-digit loop dup radix vs
applyFormatSpec). Evaluate consolidation where it removes a real drift hazard.
</content>
</invoke>
