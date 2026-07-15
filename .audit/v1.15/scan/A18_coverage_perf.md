# A18 Coverage + Perf (v1.15 cross-cutting)

Scanner A18 — breadth coverage-gap map + perf-variance re-analysis on the NEW generational GC.
READ-ONLY on source/tests; benchmarks run against `build-release/ki` (`-O2`, static). Builds on
v1.12 A22, v1.13 (GC-variance floor raise), v1.14 A22 (perf re-measure), 1.12.1.

---

## Job 1: Coverage-gap map (prioritized checklist)

### Method
Enumerated **206** Kirito-visible public names: 41 builtins (`def/defSig/defKw(...)` in `runtime.hpp`)
+ `range/min/max`, and every `.fn/.kwfn(...)` across `stdlib_*.hpp` (191 module functions). Corpus:
**371** `tools/tests/scripts/*.ki`, **299** `tools/tests/errors/*.ki`, **132** `tools/tests/unit/*.cpp`,
plus examples. Counted word-boundary references per name and split by test *angle*.

### Headline: function-name coverage is EXHAUSTIVE
**Every one of the 206 names has ≥3 references** in the corpus — there is NO builtin/stdlib function
with zero or 1–2 references. The `verify_*.ki` / `labx_*.ki` / `deep_*.ki` / `r{4..11}_*.ki` families
(prior rounds) give per-function happy-path coverage across the whole surface. So the breadth gap is
**not "which function is untested"** — it is **"which function's ERROR / bad-input path is untested"**.

### The real gap: error-path (adversarial) coverage is CONCENTRATED
142 of the 206 names never appear in the dedicated `tools/tests/errors/*.ki` diagnostic-text suite.
Most of those DO get bad-input coverage via `try/catch` asserts inside `labx_/deep_` scripts (checked
by thrown-error-string presence). The genuinely thin spots — where a distinctive failure string has
**0 test files** — are the priority checklist:

| Priority | Surface | Missing angle | Evidence |
|---|---|---|---|
| **HIGH** | `crypto.aesdecrypt` tamper detection | AES-GCM wrong-tag / wrong-key → `"authentication failed"` | **0** test files match `"authentication failed"` — the whole AEAD integrity contract (CLAUDE.md: "a failed tag throws … never garbage") has no negative test. |
| **HIGH** | `int.modinv` non-coprime | modular inverse of non-coprime args → error | **0** test files match `"not invertible"`/`"no modular inverse"`. Only 1 script uses `modinv` at all (happy path). |
| **MED** | `crypto.rsaverify` / `ecverify` | verify returns False on a bad signature; verify with wrong key; malformed-PEM `rsaimport`/sign | each in exactly 1 script, happy-path only; no tamper/wrong-key case. |
| **MED** | `crypto.x509parse` | malformed / truncated / non-cert PEM | 1 script, happy-path parse only; no reject-garbage test. |
| **MED** | `int.fromstring` | bad base, out-of-alphabet digit, empty string | 12 scripts use it, **0** co-locate a catch/error; the invalid-digit reject path looks untested. |
| **MED** | tensor `einsum`/`tensordot`/`searchsorted`/`contract` | bad subscript string / mismatched axes / non-sorted input | heavily used happy-path (10–16 scripts each) but no bad-axis/bad-subscript negative test found. |
| **LOW** | `net` resolution `getaddrinfo`/`gethostbyname` | unresolvable host, bad family/type string, out-of-range port | present in 3 scripts, no error-string co-location (network-gated, hard to test deterministically — but the *validation* rejects can be tested offline). |
| **LOW** | `crypto.rsagenerate`/`ecgenerate` | invalid bits / unknown curve name | 1 script; no bad-parameter reject test. |

Well-covered on the error angle (NOT gaps, listed to bound the map): `"singular"` (26 files),
`"shape"` mismatch (92), `"math domain"` (37), `"invalid literal"` (3), matrix `inv`/`det` non-square.

### Native-object methods (not in the `.fn` list — dispatched via `getAttr`)
Socket (`recvall/recvfrom/sendto/setsockopt/getsockopt/starttls/detach/fileno/shutdown/getpeername/
getsockname`), Tensor (`argsort/broadcastto/cumprod/expanddims/swapaxes`), ComplexMatrix (`hermitian`)
all resolve and appear in tests. These are a *separate* surface my `.fn` enumeration didn't cover; the
per-subsystem agents (A11 tensor/matrix, A12 net) own their depth — flagged here only so the fix phase
knows the breadth map's `.fn` list is not the whole method surface. Socket option *validation* rejects
(`setsockopt("bogus", ...)`, out-of-range port) and `detach`-after-`detach` are the likely thin methods
(A12's remit).

### Job-1 bottom line
Add ~6–8 focused negative tests to `tools/tests/errors/` (or a `verify_crypto_errors.ki` /
`verify_int_errors.ki`): AES bad-tag, RSA/EC verify-fail + wrong-key, x509 garbage-PEM,
`modinv` non-coprime, `fromstring` bad-digit, one tensor bad-axis. That closes the only systematic
breadth gap left after three exhaustive rounds.

---

## Job 2: Perf-variance analysis (the NEW generational GC)

### TL;DR
The generational GC (v1.15) did **not** reduce the per-operation variance the user complains about,
and its design is **self-defeating for stability as currently tuned**: because the collection trigger
(`gcThreshold_`, adaptive `live*4`, floor **65536**, **no upper cap**) governs BOTH minor and major
cadence, the "nursery" holds ~50k–120k young objects, so a *minor* scans ~50k slots and costs ~2 ms —
nearly as much as a ~4 ms *major*. The result is a **bimodal pause pattern** (frequent ~2 ms minors +
occasional ~4 ms majors) instead of the generational ideal (tiny frequent minors). Measured intra-run
per-rep **CV is 0.5–3.7** on alloc-heavy loops — as bad as the v1.12 pre-generational baseline.

### Measurements (build-release, this machine — noisy shared host, so read CV not absolutes)

**Intra-run per-rep CV** (reported by `compare_bench.ki` itself; excludes process startup — this is the
per-operation jitter a user timing calls/short scripts sees):

| workload (2000×400) | CV samples |
|---|---|
| `sum_loop` | 0.55, 0.55, **1.17** |
| `dict_ops` | 0.63, 0.74, 0.79, **3.66** |
| `string_ops` | **1.6–2.1** |
| `sort` (few new boxes) | 0.13–0.55 |

**Across-process WALL CV** (12 runs, python-timed around the process) is by contrast **already low**
— adaptive: `dict_ops` 0.073, `sum_loop` 0.020, `string_ops` 0.021. Whole-run wall time averages the
spikes out. **This reconciles v1.14 A22's "variance is fine (2–4%)" verdict: it measured whole-run wall
(the C++ `bench` harness), which is smooth; the per-operation jitter that reads as "unstable" was not
measured and is NOT fine.** Use intra-run CV as the stability metric going forward.

**GC-stats vs threshold** (`--gc-stats`, `dict_ops 2000×400`) — the key diagnostic:

| threshold | minors (ms) | majors (ms) | live | young | **arena capacity** | intra CV |
|---|---|---|---|---|---|---|
| T=20000 | 72 (37.6) | 9 (12.7) | 23240 | 2080 | **40934** | ~0.38 |
| T=65536 | 21 (41.9) | 3 (8.3) | 63270 | 49216 | 85372 | ~0.66 |
| adaptive (`live*4`) | 21 (42.4) | 3 (11.6) | 29885 | 14752 | 100517 | ~0.7–1.0 |
| T=131072 | 10 (72.0) | 2 (18.8) | 57244 | 49216 | **150907** | ~1.0 |

Two facts jump out:
1. **Total GC cost is ~flat across thresholds (~50 ms)** — it amortizes correctly; the variance is the
   *distribution* of that cost into fewer/bigger vs more/smaller pauses. Lower threshold → smaller,
   more evenly-spaced pauses → **lower intra-run CV and 2.5× smaller arena** (40934 vs 100517 vs 150907).
2. **A premature-promotion feedback loop.** `live` inflates with the threshold (23240 → 63270 for the
   *same* workload) because promote-on-first-survival (`kGcOldAge = 1`, object.hpp) promotes any
   loop-temporary that happens to be alive during a minor; rarer collections promote more, so more dead
   objects float as "old" until the next major. That inflated `live` then feeds `gcThreshold = live*4`,
   pushing the threshold (and arena, and pause size) **higher still** — a self-reinforcing ratchet.
   v1.13's floor raise (20000→65536) and the un-capped `live*4` sit on the wrong side of this loop.

### Confirmed NON-issues / already-done
- **First-collection-is-major warmup**: already implemented (`minorsSinceMajor_ = kMajorEveryMinors`
  initial, vm.hpp:463 — "start high => the first collection is a major"). Good; no change needed.
- **`young_` compaction retains capacity** on `sweepYoung` (arena.hpp:119) — good.
- The `young_.push_back(slot)` per alloc is a single amortized-O(1) append; not a variance driver
  itself (but see reserve below for the *reallocation* spikes).

### Ranked LOW-RISK stabilizers

**S1 — Cap the adaptive threshold + shrink the multiplier. [LOW risk / HIGH reward]**
`gcThreshold_ = std::max(kGcThresholdFloor, arena_.liveCount() * 4)` (vm.hpp:193) has **no upper
bound**, so a workload that transiently inflates `live` balloons the arena to 100k–150k slots and grows
every pause. Change to e.g. `std::clamp(liveCount()*2, kFloor, kCap)` with `kCap≈131072` and lower the
floor toward ~32768. Data: this bounds arena at ~40–85k, keeps total GC cost flat, and cuts intra CV
(dict_ops ~0.7→~0.38 at the low end). *Risk:* one-line policy constant, no semantic change; the only
downside is slightly more frequent collections on huge-live workloads (throughput was actually equal or
better at low threshold here). *Measure:* `--gc-stats` capacity + `compare_bench.ki` intra CV at fixed
thresholds (already scripted above); pick the knee of the CV-vs-capacity curve.

**S2 — Reserve arena vectors upfront. [LOWEST risk / MODEST reward]**
`ObjectArena` never `reserve()`s `slots_`, `young_`, or `free_` (confirmed — no reserve in arena.hpp).
`slots_` (a `vector<Slot>`, each holding a `unique_ptr`) grows by geometric doubling to the ~100k
high-water, i.e. ~17 reallocations, the last few **moving 25k/50k/100k `unique_ptr`s** — unpredictable
early-run spikes on top of any GC pause. Add `slots_.reserve(8192); young_.reserve(8192);` in the ctor.
*Risk:* pure capacity hint, zero semantic change, cannot regress correctness. *Reward:* removes the
handful of early realloc spikes (mostly startup / first alloc burst — helps short scripts most, which
is exactly the user's perceived-instability case). *Measure:* deterministic — count/​time the
capacity-doubling events; compare first-100ms jitter of a short script.

**S3 — Give minors their own SMALL cadence (decouple from `gcThreshold`). [MED risk / HIGH reward]**
This is the real fix and the whole point of a generational GC. Today `alloc()` uses one threshold for
both, so the nursery is ~50k–120k and minors cost ~2 ms. A separate `kMinorThreshold ≈ 8192–16384`
(fixed, cache-friendly) makes minors tiny (O(8k young)) and frequent → the pause stream becomes many
sub-100µs blips instead of ~2 ms lumps, collapsing intra CV. *Risk:* medium — touches the `alloc()`
cadence branch and needs the full debug/asan/tsan pass, but no data-structure change (the machinery
exists). *Reward:* highest — directly delivers the generational promise the current tuning throws away.
*Measure:* `gcMinorNanos()/gcMinorCount()` (per-minor pause should drop ~10×) + intra CV.

**S4 — Promotion policy `kGcOldAge = 2` (survive TWO minors before promoting). [MED risk / MED reward]**
Breaks the nepotism loop in S1's diagnosis: a loop-temporary alive during one minor no longer becomes
permanent old-gen floating garbage. The `sweepYoung` code **already supports** `kGcOldAge > 1` (the
`if (s.obj->gcYoung()) young_[keep++] = slot;` path, arena.hpp:131), so this is a constant flip + the
`resetRemembered` wholesale-clear comment (arena.hpp:59) that currently *relies* on age==1 promoting
everything — that assumption must be revisited (a surviving-but-still-young object keeps its old→young
edges live, so the remembered set can't be blindly cleared). *Risk:* medium — the remembered-set
soundness note makes this more than a one-liner; needs the write-barrier soak (`--gc-threshold 1`) + tsan.
*Reward:* smaller `live`, smaller majors, tames S1's ratchet. Do S1 first (cheaper, bounds the same
symptom); reach for S4 only if S1+S3 leave a memory/live-inflation tail.

**Not recommended (out of the low-risk envelope):** incremental/concurrent major collection; a moving
nursery; per-value refcounting. All are large rewrites the request explicitly excludes.

---

## Summary

**Top coverage gaps (Job 1):** function-name coverage is *exhaustive* (all 206 builtins/stdlib fns
≥3 test refs) — the only systematic gap is **error-path** coverage for a few modules absent from the
`tools/tests/errors/` suite: **crypto AES bad-tag (`"authentication failed"` — 0 tests)**, **crypto
RSA/EC verify-fail + wrong-key**, **crypto x509 garbage-PEM**, **`int.modinv` non-coprime (0 tests)**,
**`int.fromstring` bad-digit**, **tensor `einsum`/`tensordot` bad-axis**. ~6–8 negative tests close it.

**Top perf stabilizers (Job 2):** the generational GC did NOT fix per-operation variance (intra-run
CV still 0.5–3.7) — its trigger governs both minor and major cadence, so "minors" scan a 50k+ nursery
(~2 ms) and the pauses are bimodal. Whole-run wall CV is already low (~2–7%), which is why v1.14 read
variance as "fine" — it measured the wrong metric. Ranked fixes: **S1 cap+shrink the adaptive threshold
(one constant, bounds arena 2.5× and cuts CV — LOW/HIGH)**, **S2 reserve arena vectors (zero-risk,
removes early realloc spikes)**, **S3 give minors a small fixed nursery cadence (MED/HIGH — the real
generational win)**, S4 `kGcOldAge=2` to break the premature-promotion ratchet (MED, do after S1). All
measurable with `--gc-stats` (arena capacity, per-minor nanos) + `compare_bench.ki` intra-run CV.
