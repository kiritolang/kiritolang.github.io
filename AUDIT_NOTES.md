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

### IO / net / sys / time / proc (stdlib_io/net/sys/time.hpp, proc_compat.hpp, net_compat.hpp)
- `NEW MED` — `stdlib_net.hpp:510-512` — **HTTPS response body unbounded (OOM)**: the plain-TCP path
  uses `recvAll` (256 MiB cap) but the TLS path's own `while(SSL_read>0) raw.append` has NO cap. A
  chatty HTTPS server OOMs the process. Fix: apply kMaxRecvAll ceiling in the SSL_read loop.
- `NEW MED` — `proc_compat.hpp:262 (POSIX)/133,152 (Win)` — timeout doesn't kill the process GROUP; a
  lingering grandchild holding the stdout pipe blocks `tout.join()` forever → sys.shell never returns
  despite timeout=. Fix: setpgid + kill(-pgid) on timeout, or poll/deadline drains. (harder fix)
- `NEW MED` — `stdlib_net.hpp:375,743-777` — redirect `Location`/URL with bare `\n` flows unsanitized
  into the next request line/Host → header injection/request smuggling driven by the server. Fix:
  strip/reject CR/LF (control chars) in parseUrl host/path + resolveUrl output.
- `NEW MED` — `stdlib_io.hpp:99-108` — **File.read(n) size-cap bypassed when cursor is past EOF**: the
  `want = min(want, endp-cur)` clamp only applies when `endp>=cur`; after `seek(past_end)` (cur>endp or
  tell==-1) the guard is skipped → `string buf(n,0)` allocs the full user n (~9e18). Fix: clamp to
  max(0, endp-cur) regardless of sign.
- `NEW LOW` — `stdlib_net.hpp:723-726` — default `timeout=0` = no socket timeout → a stalling server
  hangs the request forever. Consider a default deadline.
- `NEW LOW` — `net_compat.hpp:73-77` — send/recv length `static_cast<int>(n)`; a >2 GiB payload
  overflows int → misbehaving loop. Fix: clamp each syscall to a chunk size before the cast.
- safe (checked): dechunk bounded, gunzip FEXTRA bounded, chunk-size overflow caught, redirect count
  bounded, TLS verify no bypass, timegm/make/strptime range-checked, BytesIO/File seek overflow-guarded.

### Serialization / compression (stdlib_serde/serialize/dump/gzip.hpp, deflate.hpp)
- `NEW MED` — `stdlib_dump.hpp:138-139` & `stdlib_serialize.hpp:89-91` — **node-count allocation
  amplification (OOM)**: decode validates only `n > data.size()` then `vector<Node>(n)` (~80 B/node,
  min record 1 B) → ~80× zeroed alloc; a ~200 MB blob forces multi-GB. Fix: cap n to
  data.size()/minRecord or reserve incrementally.
- `NEW LOW(note)` — `stdlib_serde.hpp:181-245` — dump/serialize.loads is pickle-style unsafe on hostile
  bytes (instantiates arbitrary registered classes + runs `_setstate_`). Trust-boundary; document
  loudly / consider safe-mode. (design, not a quick fix)
- `NEW LOW` — `stdlib_gzip.hpp:70` — O(n²) `data.substr(pos)` copy per member on many-member gzip. Fix:
  inflate over an offset/string_view.
- `NEW LOW` — `deflate.hpp:348-353` — zlibDecompress ignores FDICT (`FLG & 0x20`) → a valid preset-dict
  stream is misparsed. Fix: throw "preset dictionary unsupported" (or skip 4 DICTID bytes).
- `NEW LOW` — `stdlib_dump.hpp:150,152` — `for(k < c*2)` uint32 wrap for a Dict/Object count
  `c >= 0x80000000`. Fix: uint64 loop bound.
- safe: INFLATE Huffman/distance bounds, rebuild id-checks, MD5/SHA/CRC/Adler vectors.

### Collections / builtins / bytes (collections.hpp, runtime.hpp Set ops, bytes.hpp)
- `NEW MED` — `runtime.hpp:875-903` — **Set.union / Set.symmetricdifference GC-sweep UAF**: when `other`
  is a String/Bytes/large range, `iterate` returns FRESH element handles stored into the not-yet-arena'd
  `result` without rooting; the final `vm.alloc` can GC and sweep them → dangling. (intersection/
  difference/issubset safe — receiver-owned elems.) Fix: `for (Handle e : *other) rs.add(e);` or
  GcPauseScope around build+alloc.
- `NEW LOW` — `collections.hpp:111,213` — Dict/Set iteration order not insertion-stable across
  delete (fum swap-on-erase) — diverges from Python's insertion order. Behavioral, not unsafe. (defer)
- `NEW LOW(DRY)` — `bytes.hpp:124` — hardcoded `256*1024*1024` repeat cap vs shared `kMaxRepeat`. Unify.
- safe: sliceIndices negative/step<0/overflow, int64↔double exact eq+hash, UTF-8 validation, fromhex.

### Kirito-modules / kpm / native glue (stdlib_kimodules.hpp, kpm/kpm.ki)
- `NEW MED` — `stdlib_kimodules.hpp:1281-1297 _infer` — readcsv mis-coerces: `Integer/Float` accept
  `0x1F`/`00123`/`1e3`/whitespace → a zero-padded ZIP or hex-ID string is silently converted, corrupting
  data pandas keeps as string. Fix: strict `^-?[0-9]+$` / plain-decimal check before converting.
- `NEW MED` — `stdlib_kimodules.hpp:682,2141` — blank CSV line → padded all-None row (pandas skips).
  Fix: skip a parsed lone-empty-field row in readcsv.
- `NEW LOW` — `stdlib_kimodules.hpp:1196` — arg.Parser treats any 2-char `-X` as an option → negative
  positional `-5` throws "unknown option". Fix: option only when `_byshort` matches.
- `NEW LOW` — `kpm/kpm.ki:547-556 cmdRemove` — package name not vetted with `safeRelPath` → `kpm remove
  ../../foo` rmtree's outside `~/.kirito/packages`. Fix: safeRelPath-check name before join/rmtree.
- safe: semver ranges/prerelease precedence, makeMethod kw binding, safeRelPath/validateManifest,
  base64, csv quoting, xml, statistics.quantiles, tabular merge/groupby/valuecounts.

---

### Numeric: tensor / matrix / complex / math (tensor.hpp, stdlib_tensor/matrix/complex/math.hpp)
- `NEW MED` — `stdlib_math.hpp:254-262` — **math.comb false "too large"**: step-wise `r*(n-k+i)`
  overflows u64 on the intermediate even when the final fits int64; `math.comb(64,32)` throws though
  `C(64,32)=1.83e18 < INT64_MAX`. Fix: `__int128` intermediate or gcd-reduce before multiply.
- `NEW LOW` — `stdlib_tensor.hpp:1337` — `norm(ord=-inf)` returns max|x| not min|x| (isinf catches both
  signs). Fix: branch `ord<0` → min.
- `NEW LOW` — `stdlib_tensor.hpp:1332-1341` — `norm` on a 2-D tensor is entrywise not induced
  (ord=1/2/inf disagree with NumPy; Frobenius default OK). Fix: doc vector-only or dispatch induced.
- `NEW LOW` — `tensor.hpp:49-56` — `checkedNumel` rejects `zeros([1e8, 0])` (per-dim cap fires before a
  later 0 axis collapses count). Fix: short-circuit to 0 if any dim is 0.
- `NEW LOW` — `stdlib_tensor.hpp:1412-1424` — einsum contraction `total` (product of all label extents)
  is only a loop bound, never size-checked → `einsum("ij,jk->ik")` with i≈k≈64Mi, j=1 → ~4.5e15-iter
  hang (DoS from small input). Fix: cap total.
- safe: checkedNumel/checkSize at every growing ctor, slicePicks count-driven, INT64_MIN-safe helpers,
  NaN-last comparator is valid strict-weak-order.

### Regex (regex_engine.hpp, stdlib_regex.hpp)
- `NEW MED` — `regex_engine.hpp:585,599,603` — **Pike-VM capture vectors deep-copied per epsilon step
  → O(program² · groups) DoS**: one input position costs O(program × numGroups); a group-dense pattern
  `"()"*99000` (~198 KB, under the 200000-instr cap) hangs on a 1-byte/empty subject. The stated
  "O(input·program)" bound is wrong. Fix: COW capture arrays (shared_ptr), cap numGroups, or a shared
  slot log. (larger fix — mitigate by capping numGroups)
- `NEW LOW` — `stdlib_regex.hpp:431-442` — module `sub`/`split` drop the `flags` arg (IGNORECASE/etc.
  unreachable one-shot). Fix: add trailing flags param.
- `NEW LOW` — `regex_engine.hpp:334-346` — `\u`/`\U`/`\x` accept lone surrogates (D800-DFFF) → dead
  pattern element. Fix: reject surrogate range.
- safe: epsilon-cycle guard (visited/gen), {n,m} caps, empty-match advance, capture OOB guards,
  anchors/boundaries/MULTILINE/DOTALL, greedy/lazy priority.

### Compiler / bytecode VM (compiler.hpp, bytecode_vm.hpp)
- `NEW HIGH (REPRODUCED CRASH)` — `bytecode_vm.hpp:399-408 unwind + compiler.hpp:289-299/471` —
  **`break`/`continue` in a `finally` reached via an exception SEGFAULTs or HANGS**. `unwind()` pushes
  the exception value onto the operand stack, so the finally's exception-path landing pad runs with
  `exc` sitting on top of the (still-live) loop iterator cursor. break/continue's fixed cursor-pop math
  assumes a clean stack → mislocates. Reproduced on build-debug/ki:
    - `for i in [1,2,3]: (try: throw "x") finally: continue` → SEGFAULT (exit 139)
    - nested for with `finally: break` → infinite HANG (exit 124)
  (return-in-finally & normal-path continue-in-finally are safe.) Fix: hold the in-flight exception in
  a dedicated VM register/stack (like `excSpan_`), not on the operand stack, so finally/handler landing
  pads always start at the clean operand level; Reraise/ExcMatch/handler-bind read it from the register.
- `NEW LOW` — `compiler.hpp:433/471/475` — a `finally` body clobbers the try's carried result
  (`try: 5 \n finally: pass` → result None not 5), contradicting the line-475 invariant. Cosmetic
  (REPL echo). Fix: save/restore result slot around the finally, or update the comment.
- safe: const-dedup keys type-prefixed, switch lowering balanced, unpack targets, try/catch/finally
  block-stack discipline, with enter/exit, GC rooting across allocating ops, slot/name aliasing.

## FIXES APPLIED (this session)
Validated (debug build + 260 golden scripts green): finally-crash, sort, Dict/Set reentrancy,
Set-union GC, format-precision, comb, io-read-cap, https-cap, serde-caps. The rest are contained,
compile-reviewed edits (user builds); the self-asserting `spec_audit_hardening.ki` re-checks them.

- `FIXED` HIGH — **break/continue/return in a `finally` reached via an exception** (SEGFAULT/hang):
  `compiler.hpp` parks the in-flight exception in a hidden `$excN` local across the exception-path
  finally body. Validated on the reproducers + all golden. Regression: `spec_finally_control.ki`.
- `FIXED` HIGH — `runtime.hpp` **List.sort iterator-invalidation UAF**: snapshot elems, sort snapshot,
  reassign after all user code runs.
- `FIXED` HIGH — `collections.hpp` + `runtime.hpp` **Dict/Set reentrant `_eq_`/`_hash_` bucket-UAF**:
  a `ProbeScope` reentrancy guard rejects nested mutation during a probe with a clean error.
- `FIXED` MED — `runtime.hpp` **Set.union/symmetricdifference GC-sweep UAF**: root iterated `other`
  elements before the trailing alloc.
- `FIXED` MED — `value.hpp` List-from-vector ctor: RootScope the elements across the alloc.
- `FIXED` MED — `runtime.hpp` **format-spec precision int-overflow (UB)**: accumulate in int64, bound
  each step.
- `FIXED` MED — `stdlib_math.hpp` **math.comb false "too large"**: 128-bit intermediate.
- `FIXED` MED — `stdlib_io.hpp` **File.read(n) cap bypass** past-EOF/unknown cursor: clamp to available.
- `FIXED` MED — `stdlib_net.hpp` **HTTPS response unbounded (OOM)**: apply kMaxRecvAll in the SSL_read loop.
- `FIXED` MED — `stdlib_dump.hpp` + `stdlib_serialize.hpp` **node-count OOM amplification**: grow the
  node vector incrementally instead of sizing to the untrusted count.
- `FIXED` HIGH — `parser.hpp` **nested f-string parse-depth escape (stack overflow)**: thread the
  expression-nesting depth into the f-string sub-parser.
- `FIXED` MED — `stdlib_net.hpp` **redirect/URL CRLF header-injection**: parseUrl rejects control chars.
- `FIXED` LOW — `runtime.hpp` `Integer("0x0x5")` doubled-prefix rejected.
- `FIXED` LOW — `runtime.hpp` String pad byte-length bound (multibyte fill).
- `FIXED` LOW — `lexer.hpp` indentation counters int→int64 (overflow).
- `FIXED` LOW — `regex_engine.hpp` `\u`/`\U`/`\x` reject lone surrogates.
- `FIXED` LOW — `deflate.hpp` zlib FDICT (preset dictionary) rejected clearly.
- `FIXED` LOW — `stdlib_dump.hpp` Dict/Object `c*2` uint32 wrap → uint64 loop bound.
- `FIXED` LOW — `net_compat.hpp` send/recv length clamped to a 16 MiB chunk (int-truncation).
- `FIXED` LOW — `tensor.hpp` checkedNumel: a 0 dim short-circuits to 0 (no false "too large").
- `FIXED` LOW — `stdlib_tensor.hpp` `norm(ord=-inf)` returns min|x| (was max).
- `FIXED` MED — `stdlib_kimodules.hpp` readcsv `_infer`: gate numeric inference on plain-decimal
  (reject 0x/0o/0b + whitespace); skip blank lines (no all-None row).
- `FIXED` LOW — `stdlib_kimodules.hpp` arg.Parser: a non-matching `-X` is a positional (negative nums).
- `FIXED` LOW — `kpm/kpm.ki` cmdRemove vets the package name (path-traversal guard).

## DEFERRED (need dedicated design + a build/sanitizer cycle — documented, not yet patched)
- MED — `dispatcher.hpp` shutdown-created waitable never aborted (deadlock); Barrier-timeout stale
  count; Semaphore over-release / Lock non-owner release. Concurrency fixes — need TSan validation.
- MED — `regex_engine.hpp` per-thread capture vectors deep-copied → O(program²·groups) DoS. Needs
  copy-on-write captures (or a numGroups cap) + benchmarking.
- MED — `parser.hpp` f-string `{…}` scanner is quote-unaware (`f"{d['a:b']}"` mis-splits). Needs a
  quote-state-tracking rescan; risky without tests.
- MED — `proc_compat.hpp` timeout doesn't kill the process GROUP (grandchild holding the pipe hangs
  the join). Platform-specific (setpgid/CreateJobObject).
- LOW — `stdlib_serde.hpp` loads is pickle-style unsafe on hostile bytes (design: safe-mode/allowlist).
- LOW — `arena.hpp` 32-bit generation wraparound; `handle.hpp` `Handle{}` aliases None (slot 0).
- LOW — `stdlib_gzip.hpp` O(n²) substr per member (needs inflate-at-offset API).
- LOW — `collections.hpp` Dict/Set iteration order not insertion-stable across delete.
- ~~LOW — `stdlib_regex.hpp` module sub/split drop the flags arg.~~ **DONE**: added a trailing
  `flags` param to one-shot `regex.sub`/`regex.split`. Test: `spec_regex_oneshot_flags.ki`.
- LOW — `parser.hpp` inline-body `return a, b` doesn't pack; `lexer.hpp` NUL-in-string = EOF.
- nit — `bytes.hpp` repeat cap duplicates `kMaxRepeat` (needs the constant moved to a low-level header).
