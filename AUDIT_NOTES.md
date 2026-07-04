# Deep audit — working notes (in progress)

Durable log of the full-codebase audit so nothing is lost across sessions. Findings are collected
from parallel subsystem-audit agents + static analysis, then verified here, then fixed with tests.
This file is temporary scaffolding and will be removed once fixes land.

Status legend: `NEW` (reported, unverified) · `CONFIRMED` (verified real) · `REJECTED` (false alarm)
· `FIXED` (patched + test).

---

## Static analysis (clang 18 `--analyze`, checkers: core,cplusplus,deadcode,security,unix,nullability)

Ran over the whole interpreter via `main.cpp` (includes `kirito.hpp`). Only 2 warnings in our headers:

- `NEW` low — `stdlib_tensor.hpp:2236-2237` — `security.FloatLoopCounter`: a `double` used as a loop
  counter (linspace-style). Float loop counters can drift/skip; review whether an integer counter is
  used to bound iterations (likely already integer-bounded → false positive, verify).

No core/security/nullability defects flagged elsewhere. The codebase is `-Wconversion`-clean and
already hardened against INT64_MIN UB (explicit unsigned-magnitude handling everywhere).

## Cross-cutting DRY (single-source-of-truth)

- `NEW` low — float→int64 checked conversion is duplicated. `stdlib_math.hpp:29 toInt64Checked(d, who)`
  is the canonical helper (used by `math.floor/ceil` and `time` add/sub), but the identical
  NaN/inf/`>=2^63` guard is re-implemented inline in `runtime.hpp:2692-2695` (Integer ctor),
  `runtime.hpp:2885-2886` (round), and `stdlib_time.hpp:328-330` (datetime). The `2^63` literal
  `9223372036854775808.0` appears ~8 times. Consider promoting a single `fitsInt64(double)` /
  `toInt64Checked` to a shared header (`common.hpp`) and routing all sites through it, so the range
  bound has one definition. (Behavior is currently consistent — this is maintainability, not a bug.)

---

## Subsystem agent findings

_(appended as each parallel audit agent returns; verified below)_

### Runtime operators/dispatch (runtime.hpp)
- `NEW HIGH` — `runtime.hpp:538-558` — **List.sort(key=f) iterator-invalidation UAF**: sort binds a live
  ref to `elems` and range-iterates while calling the user `key` fn; if `key` appends to the same list
  the vector reallocs → UAF. Unlike apply/sorted/min/max which snapshot. Fix: snapshot `src = elems`
  first, build tagged, assign back guarding size. Repro: key fn does `xs.append(0)`.
- `NEW MED` — `runtime.hpp:2538-2545` — **format-spec precision signed-int overflow (UB)**: `int precision`
  accumulates `*10 + digit`, bound check `> kMaxRepeat` happens AFTER the multiply → `format(1.5,".2345678901")`
  overflows INT_MAX. Fix: accumulate in int64, or check bound before multiply. (width is size_t → safe.)
- `NEW LOW` — `runtime.hpp:2708-2736` — `Integer("0x0x5")` returns 5: after consuming our `0x` prefix,
  `stoull(...,16)` re-accepts an embedded `0x`. Fix: reject embedded prefix.
- `NEW LOW` — `runtime.hpp:1420-1427` — ljust/rjust/center guard `width>kMaxRepeat` on code-points but
  fill may be 4 UTF-8 bytes → buffer up to ~4×bound. Fix: bound `pad*fill.size()`.

### Value model + GC (value.hpp, collections.hpp, arena.hpp, handle.hpp)
- `NEW HIGH` — `collections.hpp:118-125 set / 220-227 add / 186-196 remove` — **reentrant _eq_/_hash_
  invalidates a live bucket ref → UAF/heap corruption**: `auto& bucket = buckets[hash]` cached, then
  `probeBucket` runs user `_eq_` which can mutate the same Dict/Set → rehash/realloc → dangling bucket →
  emplace_back corrupts heap. Repro: class with `_hash_`→0 and `_eq_` that does `d["x"]=1`. Fix: re-lookup
  bucket after probe, or reentrancy guard.
- `NEW MED` — `value.hpp:538-542` — `List(vm, const vector<Handle>&)` ctor lacks RootScope across its
  `alloc` (sibling init-list ctor has one). A GC during alloc can sweep un-rooted elements → dangling.
  Fix: RootScope the handles before final alloc.
- `NEW LOW` — `arena.hpp:44,57` — 32-bit generation wraparound after 2^32 slot reuses → stale handle
  accepted as live (long-running server). Fix: retire near-wrap slots, or widen to 64-bit.
- `NEW LOW` — `handle.hpp:15-19 + vm.hpp:39` — default `Handle{}` == `none_` (slot0,gen0); an
  uninitialized handle silently derefs to None instead of throwing. Fix: reserve slot 0 as invalid.

### Concurrency (dispatcher.hpp, stdlib_parallel.hpp)
- `NEW MED` — `dispatcher.hpp:572-586 shutdown + 610-619 makeWaitable` — **waitable created AFTER the
  abort fan-out is never aborted → shutdown deadlock**: a worker that creates a Queue/Event/Lock after
  phase-1 abort then blocks forever; phase-2 join() hangs. Fix: makeWaitable (holds registryMutex_)
  should abort/throw immediately when shuttingDown_.
- `NEW MED` — `dispatcher.hpp:268-273 Barrier::wait timeout` — broken generation not cleared: timeout
  sets `brokenGen_` but leaves `count_` stale → a later single arrival can trip "last party" and
  self-heal the barrier off a stale count. Fix: full resetBarrier bookkeeping on timeout.
- `NEW LOW` — `dispatcher.hpp:515-522,550-553` — unsynchronized `libPaths_`/`maxCallDepth_` read on
  worker threads (outside registryMutex_). TSan race if set-during-spawn. Fix: guard or document.
- `NEW LOW` — `dispatcher.hpp:228-231 Semaphore::release / 154-159 Lock::release` — unbounded
  over-release (no ceiling) and non-owner lock release (ignores owner_). Fix: bound/owner checks.

### Front end (lexer.hpp, parser.hpp)
- `NEW HIGH` — `parser.hpp:893-894 parseEmbedded` — **nested f-strings escape the parse-depth guard →
  native stack overflow**: each `{expr}` spawns a fresh Parser with `exprDepth_=0`, so kMaxParseDepth
  never accumulates across f-string nesting → SIGSEGV on deeply nested quote-alternating f-strings.
  Fix: thread exprDepth_/a global recursion counter into the sub-parser, or cap f-string nesting.
- `NEW MED` — `parser.hpp:924-941 parseFString` — f-string `{…}` scanner is quote-unaware: the
  brace-match and `:`-spec split ignore string literals inside the expr → `f"{d['a:b']}"` and
  `f"{d['}']}"` (documented-supported) fail to parse. Fix: track quote state in both loops.
- `NEW LOW` — `lexer.hpp:129-136` — indentation width counters are `int`; `wide += 8-(wide%8)` per tab
  can overflow signed int (UB) on a huge leading-tab run. Fix: int64/size_t + cap.
- `NEW LOW` — `lexer.hpp:113-116,297-299` — embedded NUL byte in a string literal is treated as EOF →
  spurious "unterminated string". Fix: drive by `pos_ < src_.size()` not a '\0' sentinel.
- `NEW LOW` — `parser.hpp:735-737` — inline-body `return a, b` uses parseExpr (returns `a`), unlike
  block-body return which parseValueSeq-packs into `[a,b]` — behavior inconsistency. Fix: parseValueSeq
  in the inline-return arm.
- clean: ast.hpp, resolver.hpp, locals.hpp, analyzer.hpp.
