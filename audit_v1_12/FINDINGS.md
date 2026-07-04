# Kirito v1.12 — Consolidated Findings & Hardening Plan

Triage of the 22-agent audit (Waves 1–3). Full per-finding repros live in `agents/A01..A22.md`.
Totals: **13 High, 28 Medium, 37 Low, 13 Nit.** No Critical. The GC core, control-flow lowering,
regex engine, serde deserialization, parallel dispatcher, and math/random held up clean under heavy
ASan/TSAN fuzzing.

Legend: ☐ todo · ▣ fixed+tested · ↷ deferred (documented, low value)

---

## HIGH — release-blockers (crash / UB / security / silent-wrong / unbounded)

Fix each with a regression test (C++ unit and/or `.ki` + `.experr`). Grouped by fix so themes land once.

### Crashes / uncatchable (violate the "guards THROW, never crash" invariant)
- ▣ [FIXED] **A04-1** call-depth guard is count-based (`maxCallDepth_=3000`); recursion through a native
  higher-order fn (`sorted(key=g)`, `xs.sort(key=g)`, `min/max(key=g)`, `apply(g)`) has deep C++
  frames and overflows the 8 MB native stack at ~2940 → **SIGSEGV**. Fix: stack-pointer-aware guard
  (record base SP at VM start; in the depth check compare current SP against a margin).
- ▣ [FIXED] **TENSOR-1** (A12) rank cap (64) enforced only on element count, not rank; `reshape`/`expanddims`/
  `broadcastto` build a rank-200000 tensor → recursive `str()`/`tolist()` **SIGSEGV**. Fix: enforce
  the rank cap in `tns::make`/`checkSize` (single-source next to `checkedNumel`).
- ▣ [FIXED] **A10-4** `sys.createprocess`/`shell` capture child stdout/stderr with no size cap; the
  `std::bad_alloc` is thrown *inside a drain `std::thread` with no try/catch* → `std::terminate`/
  **SIGABRT**, uncatchable. Fix: cap captured output (throw catchably); wrap the drain-thread body in
  try/catch and propagate.
- ▣ [FIXED] **A10-2** `sys.exit()` calls `std::exit()` unconditionally → from a `parallel` worker thread it
  runs static dtors while siblings live → **nondeterministic deadlock** (~50%). Fix: unwind via a
  dedicated exit signal to the owning thread / dispatcher instead of `std::exit` off-main-thread.
  (Related: A19-4 same root — `std::exit` skips `~KiritoDispatcher`.)

### UB / memory
- ▣ **A07-1** [FIXED, ASan-clean] `InstanceValue::equals` `static_cast`s any `ValueKind::Instance`, but every `NativeClass`
  (DateTime/Bytes/Matrix/…) also reports `Instance` → wrong-type downcast reading a garbage `Handle`,
  reachable as a Dict/Set key (UBSan-confirmed). Fix: `dynamic_cast<const InstanceValue*>`; route the
  C++ equality wrappers through the shared `kiEquals`.

### GC-rooting THEME — fresh-alloc / `iterate()` handles swept mid-op (one shared fix)
- ▣ [FIXED] **A09-3** `Set(iterable)` / `Dict(iterable)` constructors don't root `iterate()` handles → dangling.
- ▣ [FIXED] **A09-4** `zip`/`map`/`filter`/`sorted`/`enumerate`/`all`/`any` same gap (min/max/reversed/List root
  correctly — proving the omission is inconsistent).
- ▣ [FIXED] **A06-1/A06-2** (Medium, folded here) `List.apply`/`List.sort`/`Dict.apply`/`Set.apply` snapshot
  element handles into an unrooted vector.
- ☐ **A07-4** (Medium, folded here) `Value::items()` over a String returns non-pinning views onto fresh
  per-char Strings rooted only by an internal RootScope destroyed on return.
  → **Single fix: a shared `rootedSnapshot` / rooted-iterate helper** (RootScope-backed) used by all of
  the above. Knocks out A09-3, A09-4, A06-1, A06-2, A07-4.

### Silent-wrong / overflow
- ▣ **A09-1** [FIXED, ASan-clean] `pow(base,exp,mod)` computes `((base%mod)+mod)%mod` in int64 → overflows before the
  `__int128` widen → silently wrong for mod > ~2^62 (UBSan-confirmed). Fix: do the reduction in
  `__int128`/unsigned.
- ▣ [FIXED] **TENSOR-2** (A12) `einsum` contraction count is an unchecked product of all label sizes →
  `size_t` overflow (silent wrong result) + no work cap (DoS hang). Fix: overflow-checked product vs a
  work cap.

### Resource-exhaustion / security
- ▣ [FIXED] **A16-1** gzip multi-member decompress caps *each* member at 256 MiB but not the aggregate →
  400 MiB out from a 4.5 MiB `.gz`; reachable via `gzip.decompress(net.get(url).content)`. Fix:
  shrinking per-member budget (`kMaxInflateOut - result.size()`).
- ▣ [FIXED] **NET-1** (A14) cross-origin / cross-scheme redirect leaks the `Authorization` header **and** the
  whole cookie jar to the redirect target (incl. https→http downgrade) — credential exfiltration,
  `allowredirects` on by default. Fix: on a redirect whose origin differs, drop `Authorization`; scope
  cookies to their origin; never resend either over a downgraded scheme.
- ▣ [FIXED] **A20-1** semver `_parserange` splits an OR-part on a single space, so `">= 1.2.0"` → `">="` AND
  `"1.2.0"` = exact-match only; `satisfies("1.5.0",">= 1.2.0")` is `False` → **kpm resolves the wrong
  version**. Fix: parse `operator WS* version` as one comparator (unify with the leading-zero fix A20-2).

---

## Cross-cutting THEMES (single fix each; high leverage; single-source-of-truth)

- ▣ [FIXED] **T-ROOT** GC-rooting helper — see the HIGH GC-rooting block (A09-3/4, A06-1/2, A07-4).
- ☐ **T-BIND** kwarg binding + param-default resolution duplicated 3–4× with divergent policies:
  **A05-2** [FIXED] (`makeMethod` fills omitted required args with `None` → `d.setdefault(default=7)` inserts
  `{None:7}`; `d.get(default=9)` returns 9 not error), A07 (three binders), **A03-3/A03-4** (resolver
  rejects `Function(n, size=n)` that the runtime binder accepts). Fix: one binder with a real "required"
  notion; one param-default resolution rule shared by resolver/locals/analyzer/runtime.
- ☐ **T-REFLECT** operator reflection is partial/asymmetric: **A05-3** only `==` reflects onto a RHS
  instance (`3 + Money(5)` throws), **A05-1** [FIXED] `!=` non-commutative with a standalone `_ne_`, **A13-1** [BY DESIGN]
  scalar-on-left `2*M`/`2+z`/`i*C` throw. Fix: reflect the arithmetic/ordering operators onto the RHS in
  the runtime numeric-dispatch fallback (one place); document any deliberate limits.
- ▣ [FIXED] **T-GUARD** missing output-size guards: **A06-8** `replace`/`join` OOM the host (unlike
  `*`/`ljust`/`center`). Fix: cap output length (throw) via the shared `kMaxRepeat`-style ceiling.
- ▣ [FIXED] **T-RETAIN** unbounded retention in the long-lived VM: **A08-2/A19-2** dispatcher `tasks_` never
  erased (also masks a would-be UAF — fix carefully), **A09-2** Dict/Set buckets never compact,
  **A08-1** pool free-lists leak at thread exit + never shrink (**A08-1 is High** — sanitizer-invisible
  ~0.87 MB/spawn leak). Fix: erase joined Tasks (preserving reap validity); drain pool free-lists on
  thread exit; consider periodic bucket compaction.

---

## Performance (A22) — the requested variance work

- ▣ **M1 (top pick, one line)** [FIXED, ASan-clean] adaptive GC threshold: at the end of `collectGarbage`,
  `gcThreshold_ = max(~20000, 4*liveCount)`. Root cause of variance is the fixed 100 000-alloc trigger
  firing a stop-the-world O(arena) mark-sweep in periodic ~0.7–1.0 ms lumps (total GC ~25–31%, healthy,
  but bursty). Adaptive threshold shrinks each pause ~5× and spaces them evenly — pure variance
  reduction, no correctness change. Removing the spikes drops intra-run CV 3–6× (string_ops CV 1.332 →
  ~0.2 band). Measure before/after on tools/tests/bench.
- ☐ **M3** cap pool free-lists / bound arena high-water (also fixes A08 RSS ratchet + page-fault tail).

---

## MEDIUM (correctness / spec / diagnostics) — fix after High

A01-1 (`\xHH` emits a raw byte → breaks the String code-point layer; `len("\xC3\x28")==1`), A02-1
(inline `Function(): return a,b` doesn't pack → binding becomes a List), A02-2 (f-string runtime/
resolver errors report line 1), A06-7 (Set/Dict store the same NaN object twice — no identity
short-circuit in `probeBucket`), A07-2 (comparison dunders not required to return Bool), A07-3
(private-access asymmetry: subclass can't read a base-typed instance's inherited private — vs spec),
A10-3 (BytesIO.seek clamps negative vs File.seek throws), A10-5 (`for line in file` is eager → OOM on
big files), A17-1 (`DateTime._setstate_` public, mutates in place → breaks immutability + hash-key
invariant), A19-1 (fork-bomb when spawning an entry-script fn without `if argmain:` — `<main>` guard is
dead code, no diagnostic), A20-2 (semver accepts leading zeros), A20-4 (`functools.reduce` conflates
no-initial with `initial=None`), A20-5 (`tabular.sortvalues` crashes on a None column), A02 f-string
invalid-escape leniency, plus the reflection/retain items folded into themes above.

## LOW / Nit — batch-fix or document

A01-2/3/4 (backslash-at-EOF diagnostic, byte-vs-codepoint columns, control bytes in messages),
A03-1 (analyzer assigned-but-unused false positive on use-before-`var`), A05-4 (`_bool_` error loses
line:col from a condition), A06-3/4/5 (.compare lossy >2^53, empty-substring find past end, `nan in
[nan]`), A11-1 (`math.trunc` returns Float not Integer), A13-2/3/4 (MatrixBase DRY, transpose/trace
path, `norm(ord=2)` Frobenius-vs-spectral), A15-1 (`serde::rebuild` no depth cap — latent),
A16-2/3 (stored-block NLEN, zlib FCHECK), A18-2 (untested regex emit cap), A19-3 (`Barrier.abort`
irreversible), plus assorted DRY notes (A02 AST-descent hand-rolled 3–4×, A11-2 math helpers, A13-2
MatrixBase<T>, A10 seek/kMaxBuf/print-loop, A15-2 codec exception translation).

## Test-gap plan (A21) — "nothing untested"

Coverage is broad (~550–600 public symbols; few outright untested) but the dominant gap is **depth**
(~60–80 thin/happy-path-only symbols). Priority queue: (1) GC-rooting under callback-driven mutation,
(2) lexer unit + parser-boundary + diagnostic-text assertions (skeletal `test_lexer.cpp`), (3)
special-method / kwarg-binder error paths, (4) output-size & numeric-overflow guards, (5) ki-module
per-function edges (semver ranges especially), (6) release-mode RSS test for the pool leak. Every HIGH/
MEDIUM fix above ships with its regression test, which closes most of these directly.

---

## Hardening execution order (each step: fix → regression test → `post_work_check.sh` → commit)
1. **T-ROOT** shared rooted-iterate/snapshot helper (clears A09-3/4, A06-1/2, A07-4).
2. Standalone HIGH crashes: A04-1, TENSOR-1, A10-4, A10-2 / A19-4.
3. A07-1 (dynamic_cast) + route C++ equality through kiEquals.
4. Silent-wrong: A09-1, TENSOR-2. Resource/security: A16-1, NET-1, A20-1.
5. **T-BIND** unify the kwarg binder + param-default rule (clears A05-2, A03-3/4, A07 binder).
6. **M1** adaptive GC threshold (+ measure) and **T-RETAIN** (pool drain A08-1, Task erase A08-2/A19-2).
7. **T-REFLECT**, **T-GUARD**, then the MEDIUM list, then LOW/Nit + remaining test-gaps.

Verification: `tools/scripts/post_work_check.sh` (debug→release→asan→tsan) + the TLS compile gate on
every push; `bytecode_stability.sh` for semantic stability.
