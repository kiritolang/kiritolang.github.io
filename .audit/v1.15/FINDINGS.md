# Audit v1.15 — triaged findings

Full-codebase deep audit on the 1.15.0 base (new generational GC + new function/class serialization).
18 read-only scanner agents (A01–A18), each owning a `scan/AXX_*.md`. This file is the triaged roll-up.

## A19-1 [HIGH] — found while landing the perf work, NOT by the 18 scanners

**Nine unrooted-temporary sites across five files: a value must be rooted the MOMENT it is made, not
once its owner is arena-reachable.** Every one built a container (or a native method's signature)
OUTSIDE the arena and allocated into it — until that container is itself `alloc`'d, nothing traces
what it holds, so the next allocation collects the previous contents and the finished object hands
out a dangling handle. All threw `dangling handle (stale generation)`; a different allocation pattern
could reuse the slot instead and return a silently wrong value.

| Site | Reachable from |
|---|---|
| `makeNumericCompare` + `.compare` on Matrix / Complex / ComplexMatrix / Tensor (5) | `(5).compare(5)` — ANY call that omits a default (`rel_tol`/`abs_tol`). An all-positional call papers over it. |
| `tensor.nonzero` | `nonzero(t)[0]` — each per-axis tensor collected the last |
| `.shape` on Tensor / Matrix / ComplexMatrix (3) | a dimension **> 255** |
| `enumerate` | index **> 255** — ordinary code |

**Why five rounds missed it:** it needs a GC to land inside a few-instruction window, so it is
invisible at any normal threshold — and `--gc-threshold 1` makes it deterministic. The scanners ran
the soak, but only over tests using *small* values: `makeInt(n)` returns an INTERNED (permanently
rooted) object for n ≤ 255, so every shape/index in the suite was accidentally immune. It surfaced
here only because the S3 work ran the soak against `r7_types`/`r7_tensor`, which use int64 bounds and
300-element shapes. **`--gc-threshold 1` is only as good as the values the tests use.**

Fixed by rooting at the point of creation (`rs.add(...)`), and the four `.compare` signatures now
share one `native.hpp::toleranceSig` helper rather than four copies of the same trap. Pinned in
`test_gc_generational.cpp` (all nine, at threshold=1; they fail on the unfixed build). `ModuleBuilder::fn`
has the same latent hole — it roots defaults only across the final alloc, not while the caller builds
the signature — but is safe today because modules install during VM construction, before any GC.

## Headline verdicts

- **Generational GC write barrier: COMPLETE for the single-VM model** (the shipped/tested config) and
  the parallel model (A05). Every `children()`-traced Handle is either mutated only through a barriered
  mutator or set once at construction while young. Serde rebuild wires are all barriered (A10, verified
  at `KIRITO_GC_THRESHOLD=1`). This is the round's most important result — the new GC is correct.
- **C++ embedding API: no bugs, barrier-safe** (A15) — but the signatures DID change: A05-1 threaded
  the owning arena into `EnvValue::define`/`assignLocal`/`setAt`, `ClassValue::defineMethod` and
  `ModuleValue::setMember` (source-breaking only for a host reaching past `value.hpp` into those).
- ~~**No HIGH memory-safety / corruption findings anywhere.**~~ **WRONG — see A19-1 above.** Nine
  unrooted-temporary sites (including `enumerate` past index 255 and every `.compare` that omits a
  default) hand out dangling handles. The scanners' verdict was drawn from a soak that only ever used
  small-int-interned values, so the bug class was immune to the very test meant to catch it. The
  lesson for the next round: `--gc-threshold 1` proves nothing about values the tests never build.
  The rest of the round's findings ARE correctness/consistency/diagnostics + the two new subsystems.
- **Function-name coverage is exhaustive** — all 206 public Kirito names have ≥3 test references (A18).
  The only systematic gap is a few modules' error-path (adversarial) coverage.

## Fixed this batch (with regression tests)

| ID | Sev | What | Fix | Test |
|----|-----|------|-----|------|
| A08-1 | MED | `int.BigInt(0) ** (0-1)` returned `inf` — `bigint::powOp` skipped the zero-base guard every sibling path has | added the guard, same message as native `Integer**neg` (single source) | `errors/bigint_zero_neg_pow.ki` |
| A06-1 | LOW | `Dict.remove(unhashable)` gave "key not found" not "unhashable type" (unlike set/find/Set.remove) | call `requireHashable` | `errors/dict_remove_unhashable.ki` |
| A01-3 | MED | lexer `op()` default spliced a raw non-ASCII/NUL byte into the message → corrupt/truncated diagnostic | render as `\xHH` | `errors/lexer_nonascii_byte.ki` |
| A05-2 | LOW | `resetRemembered`'s wholesale clear is sound only for `kGcOldAge==1` | `static_assert` guard | compile-time |
| S2 (A18) | perf | early-run realloc spikes from growing the arena vectors from zero | `ObjectArena` reserves slots_/young_ upfront (zero-risk) | (perf; no behavior change) |
| A02-1 | MED | source-capture swept trailing comments/blank lines into a serialized Function/class | lexer records each token's extent; capture anchors on the last content token | `spec_serialize_functions.ki` (5 checks) |
| A13-1 | MED | crypto sign/verify never checked the key's family — `rsasign(ecKey)` silently signed ECDSA | `requireKeyType` guard in the shared `pkeySign`/`pkeyVerify` | `test_crypto.cpp` (9 checks) |
| A12-1 | MED | a `net.Session`'s cookies went to every host it talked to, not just the one that set them | per-cookie origin host recorded in `SessionVal`; `sessionDo` filters by it | `r7_net_regex.ki` (4 checks) |
| A10-1 | MED | `loads` executes a blob's code, undocumented | docs security section + CLAUDE.md; no API change | `test_vm_serialization.cpp` fresh-VM pin |
| A04-1 | MED | a nested handled `try` inside a `finally` stole the blame for the outer exception's line:col | park the span across the finally like the value (`Save`/`RestoreExcSpan`) | `errors/finally_nested_catch_span.ki` |

## Deferred — real findings, follow-up batch (documented here so nothing is lost)

Each has a concrete repro + proposed fix in its `scan/AXX_*.md`. Deferred because they need more care
than a low-risk one-liner and/or must be double-checked against a by-design behavior first.

**Order to work them in** (the rest of the round; this file is the source of truth for what is left):
1. **New-code correctness** — ~~A10-2~~ (done), ~~A02-1~~ (done). Bugs in surface this round introduced.
2. **Security** — A13-1 (key-type confusion), A12-1 (cross-origin cookie/Authorization leak), A10-1
   (loads executes code — docs warning).
3. **Perf variance** — S1 + S3 together; the user's key ask, see the perf section below. `kGcOldAge`
   must stay 1 (S4 is rejected — a `static_assert` guards `resetRemembered`).
4. **Diagnostics / other** — A04-1 (recurring 3 rounds), A05-1, A16-1 (confirm against the by-design
   tabular entry FIRST), A09-6, then the LOWs.
5. **Coverage gaps** — the A18 list below; `crypto.aesdecrypt` bad-tag is the highest (the AEAD
   integrity contract is entirely untested).
6. **Document + pin** — A14-1, A11-1. Accepted tradeoffs: pin the behavior, do NOT "fix" it.

Rules for each: only fix REAL triggerable bugs (check the false-positives table below first), stay
idiomatic, never break a documented contract, single-source anything reused, and pin every fix with a
regression test named for the symptom. Validate on debug + release (asan/tsan too for GC/parallel
changes) and commit in small validated batches.

| ID | Sev | What | Proposed fix | Why deferred |
|----|-----|------|--------------|--------------|
| ~~**A10-2**~~ | MED→HIGH | **FIXED** — eager class-var initializer that calls a **captured helper** fails to deserialize in a FRESH VM (helper free-vars are None during pass-0c eager init; bound only at pass 5). Masked by same-VM tests. | bind a helper's free vars before running an eager class-var initializer, or defer eager init past pass 5 | done: `bindEagerHelpers` + early-container pass 0b2; pinned by 4 empty-prelude cases in `test_vm_serialization.cpp` |
| **A10-4** | MED | split out of A10-2: an eager class-var initializer that **CALLS a helper which instantiates another class** (`var mk = Function(): return Beta()` / `class Alpha: var b = mk()`) fails in a fresh VM — `Beta` is not built when `mk()` runs, so it is still a None placeholder ("type 'None' is not callable"). Pre-existing on main; A10-2's helper-free-var binding does not reach it, because the ordering must know Beta is needed. | needs the rebuild order to know an initializer *calls* a given helper. `eagerFreeVariables` cannot: it can't tell `var m = helper` from `var v = helper()`, and ordering by mere reachability invents cycles on well-founded graphs (pinned by the Alpha/Beta case). Either record "eagerly-called" free vars at serialize time, or make class rebuild demand-driven (attempt, defer on failure, retry; genuine no-progress = cycle). | needs either a blob-format addition or a retry design whose class-body side effects must not double-run |
| ~~**A10-1**~~ | MED | **DOCUMENTED** — `dump`/`serialize` loads executes embedded code (a class body's eager init re-runs), pickle-style. Confirmed live: a fresh VM loading a class blob ran its initializer. | "Security: never load a blob you do not trust" in `docs/pages/10-stdlib.md` (+ a pointer from the `dump` section) and the CLAUDE.md serde bullet; `json` named as the code-free format for untrusted data. The optional `allow_code=False` mode was **not** added: a new API surface is a feature, and an audit loop is a patch release. | done: fresh-VM pin in `test_vm_serialization.cpp` (env-var probe: `absent` → `ran` across VMs) + a same-VM reconnect pin in `spec_serialize_functions.ki`. **Note for future rounds:** a SAME-VM `loads` does NOT re-run the body — it reconnects to the live class — so the hazard only reproduces in a fresh VM (a `.ki` golden test cannot show it). |
| ~~**A13-1**~~ | MED | **FIXED** — `crypto.rsasign/rsaverify/ecsign/ecverify` shared a generic OpenSSL path with no key-type check, so an EC key to `rsasign` yielded an ECDSA sig and verify cross-accepted the wrong family. | `requireKeyType` (`EVP_PKEY_base_id` vs the expected family) in the shared `pkeySign`/`pkeyVerify`, each entry point pinning its own family; message names expected + actual | done: 9 checks in `test_crypto.cpp` (5 fail unguarded); docs error table + stdlib page |
| ~~**A12-1**~~ | MED | **FIXED (cookies)** — `net.Session`'s jar was not host-scoped, so one origin's session cookie went to every host the Session later called (cf. the single-request NET-1 guard). | `SessionVal::cookieOrigin` (a hidden native name→host map) records the host that set each cookie; `sessionDo` sends a cookie only to that host. The public `.cookies` flat Dict is unchanged (`sess.cookies["token"]` is pinned by `r7_net_regex.ki`), so the scanner's proposed jar reshape was rejected. Scoping is by hostname, not port — matching RFC 6265 and `redirectScope`. A user-set cookie has no origin and stays unscoped. | done: 4 checks in `r7_net_regex.ki` (two origins via `127.0.0.1` vs `localhost` on one listener; the leak check fails on the unfixed build) |
| **A12-1b** | LOW | split out: `Session.headers` are still sent to **every** host, so a default `Authorization` leaks cross-origin. | **Not fixed — matches `requests`**, whose Session also sends default headers everywhere (it strips Authorization only on a cross-host *redirect*, which Kirito already does via `redirectScope`). A per-call `headers` option is the scoped alternative. Documented in `docs/pages/10-stdlib.md`. | by-design; revisit only if the docs' guidance proves insufficient |
| ~~**A04-1**~~ | MED | **FIXED after 3 rounds** (v1.13 A05-2 → v1.14 A04-3 → here) — `excSpan_` was clobbered when a `finally` contained a nested *resolved* `try/catch`, so the re-raised outer exception reported the inner line:col. | the span is now parked across the finally body exactly as the exception VALUE already was: `Op::SaveExcSpan`/`RestoreExcSpan` + `Proto::excSpanSlots`, one slot per try-with-finally, emitted around the body in `emitFinallyExc`. Slot-addressed, not a push/pop stack, so a `break`/`return`/`throw` leaving the finally early cannot desynchronize it. | done: `errors/finally_nested_catch_span.ki` pins the line:col (`:6:9:`, path-independent); reports `:9:13:` unfixed |

**Scope note (A04-1):** only `emitFinallyExc` needed it. A `with`'s exit path was suspected too, but
`excSpan_` is per-FRAME (a `BytecodeVM` member) and `_exit_` runs in its own frame, so a nested
try/catch inside `_exit_` cannot reach the caller's span. The bug needs an `unwind()` *in the same
frame* between the park and the `Reraise` — i.e. an inline, fully-handled nested try, which only the
inlined finally body has. A nested try that does NOT resolve propagates instead, so the outer `Reraise`
never runs.
| ~~**A02-1**~~ | MED | **FIXED** — source-capture over-captured trailing comments + blank lines (anchored the end on the next real token; comments emit no tokens). | the lexer now records each token's source extent in the previously-dead `SourceSpan::length`; `captureSource(startTok)` anchors on the last **content** token consumed (`contentEnd` skips the trailing Newline/Indent/Dedent structure tokens, which sit at/past the comments). Also drops an inline trailing comment on the body's last line. | done: 5 checks in `spec_serialize_functions.ki` (3 fail on the unfixed parser) |
| ~~**A05-1**~~ | MED | **FIXED** — the no-arena write barrier resolved its arena via `activeVM()` (the most recently CONSTRUCTED live VM), so with 2+ VMs on one thread it asked the wrong arena about a handle: a spurious "dangling handle" throw, or — on a slot+generation collision, which low slots readily produce — a wrong young/old answer that skipped `remember()` and let the owning VM's next minor free a still-reachable value. Embedder-only, but the embedding API explicitly allows coexisting VMs. | the arena-less overload is **deleted**, not patched, so the landmine cannot be re-armed. Every mutator takes the owning arena from its caller, matching the convention `collections.hpp` already used (`List::append(arena, h)`): `EnvValue::define`/`assignLocal`/`setAt`, `ClassValue::defineMethod`, `ModuleValue::setMember`. `InstanceValue`/`ModuleValue::setAttr` already HAD the VM and ignored it — their bodies moved to `runtime.hpp` (where `KiritoVM` is complete), as `getAttr` already does. 26 call sites, all of which had an arena in hand. | done: `test_gc.cpp` "two VMs on one thread". **Verified by emulating the old barrier at `registerGlobal`: it dies with "dangling handle (stale generation)"** — the predicted failure. *Source-breaking* for a host calling `EnvValue::define` directly (`test_r8_embed_api.cpp` did, via `static_cast` on a deref'd handle — deep internals, not the ergonomic `value.hpp` API). |
| **A16-1** | MED | `tabular.DataFrame(rows, columns=)` mismatch handling asymmetric (silent drop vs raw "index out of range"); `DataFrame(None, columns=)` ignores columns | clear DataFrame-level shape-mismatch message | **must first confirm** it doesn't collide with the "tabular ragged/index" by-design entry |
| ~~**A09-6**~~ | LOW-MED | `self._super_()(...)` throws "type 'Super' is not callable" (workaround: `_super_()._call_(x)`) | **DOCUMENTED, not fixed — deliberate.** The parent view resolves NAMED lookups, so the limitation is uniform: `_super_()[i]` ("not indexable") and every other operator dunder behave identically, and the explicit-name form works for all of them. Implementing `call` alone — the proposed fix — would make `()` work while `[]`/`+`/… still didn't, i.e. trade one rule for a list of exceptions. Implementing ALL the slots is a feature, not a patch-release fix. Recorded here if a future round wants the uniform version. | done: language guide + CLAUDE.md state the rule; `audit_langguide.ki` pins both halves (`._call_`/`._getitem_` extend the base; `()`/`[]` on the view throw) |
| ~~A10-3~~ | LOW | **FIXED** — binary `dump.loads` lacked the `std::exception`→`KiritoError` translation `serialize.loads` has | same try/catch in `dumpfmt::read` ("corrupt dump data: …"); both codecs share one rebuild, so they now report its failures alike | (latent path; no repro exists — the scan's 306-mutant fuzz never escaped one) |
| ~~A01-1~~ | LOW | **FIXED** — a leading UTF-8 BOM was a hard lex error (carried from v1.14) | stripped in `normalizeNewlines`, beside the CRLF handling it belongs with | `test_lexer.cpp`: a BOM file lexes as the plain one, col unshifted; a MID-file BOM still throws |
| ~~A01-2~~ | LOW | **FIXED** — `1.e5` lexed as `1 . e5`, a silent misparse that failed only at runtime with "type 'Integer' has no attribute 'e5'" | `.` + a well-formed exponent is a float, via one shared `exponentAt` helper. An exponent needs e/E + optional sign + digit, so `1.compare(x)` and `1 else` are untouched | `test_lexer.cpp` (float + both non-exponent forms) |
| ~~A12-2~~ | LOW | **FIXED (family label)** — `socketpair()` claimed family "inet" for a POSIX AF_UNIX pair | `netcompat::kSocketPairFamily` reports what the platform actually made (AF_UNIX POSIX / AF_INET Windows); `familyName` gained "unix" | `spec_net_primitives.ki`, platform-aware. **`getsockname` → `["", 0]` left alone**: an unnamed AF_UNIX socket genuinely has no address, and Python returns `""` too |
| ~~A12-3 / A17-1~~ | LOW | **FIXED** — a negative `timeout` silently meant "no timeout"/"instant poll" instead of throwing | `timeout >= 0` checked in `settimeout` and in `argTimeout` (the one place every parallel primitive reads it; NaN rejected too). **Behavior change**: `r10_net.ki` had pinned `settimeout(-1)` as a no-op — a characterization of the old behavior with no rationale, not a by-design entry — and was updated to expect the throw, matching Python | `spec_net_primitives.ki`, `labx_parallel.ki` (every primitive + the 0/positive cases still working) |
| ~~A13-2~~ | LOW | **FIXED** — `hashing::pbkdf2Raw` had no internal `iterations>=1` guard | added; 0 would silently degrade the KDF to a bare U1 | (unreachable from Kirito — the `hash.pbkdf2` binding already rejects it; this guards the primitive itself) |
| ~~A16-2~~ | LOW/perf | **FIXED** — `heapq.heapify` was O(n log n) (repeated push) | Floyd bottom-up via `_siftdown(heap, pos)`, now O(n). `heapreplace` shares the same helper (its call site needed the new arg — the suite caught it) | `audit_heapq.ki`: invariant on descending input, source list untouched, and the any-iterable input (range/Set/String) the old per-item loop accepted |
| ~~A17-2~~ | LOW | **RESOLVED as a bug** — `ConcurrentQueue::get` drained a buffered item before checking `aborted_` | abort now outranks a buffered item, as in every other primitive. `close()` still drains (graceful: the producer is done, don't discard what it made) — the two are different intents. Otherwise "every blocked worker unwinds on abort" held only to the depth of the queue, and shutdown ran a backlog of application code | `test_parallel_deadlock.cpp`: "abort outranks a buffered item", beside the existing "close then drain" |
| ~~A17-3~~ | LOW | **FIXED** — a `std::thread` construction failure orphaned a `Task` in the registry | erase the entry and throw a clean `parallel.spawn: could not start a worker thread` instead of letting `std::system_error` cross the native boundary | (latent: needs thread exhaustion to trigger; not unit-testable without a fault injector) |

## Document + pin (accepted tradeoffs, not fixed) — BOTH DONE

- ~~**A14-1**~~ [MED]: `(a*)*` (repeated nullable group) captured-group value diverges from Python
  (`[None]`/`['aaa']` vs `('',)`). Structural Pike-VM `visited`-dedup tradeoff (same class as RE2/Go
  `regexp`), needed for the linear-time guarantee. **DONE**: documented as an accepted limitation
  (docs/pages/10-stdlib.md, beside the backreference/lookaround rejections), pinned in
  `probe_regex_conformance.ki` — including that the MATCH itself (`group(0)`, spans) is always correct
  and that `(a*)+` / `(a)*` do agree with Python, so the divergence is Star+nullable only.
- ~~**A11-1**~~ [LOW]: `t.sum(axis=empty)` returns 0 while `mean`/`std`/`var` throw. **DONE — and the
  scan's "may be an accidental gap" reading is wrong; the rule is coherent**: a reduction WITH an
  identity returns it (empty `sum` → 0, `prod` → 1, as NumPy), one WITHOUT is a domain error rather
  than a silent NaN (`mean` is 0/0; `max` of nothing doesn't exist) — exactly the `math`-module policy
  the language already documents, and what whole-tensor `mean()` already did. Documented under "More
  reductions"; both halves pinned in `r7_tensor.ki` (sum/prod identities + mean/max/std/var throwing).

## Coverage gaps to add (A18 Job 1 + per-agent)

- ~~**HIGH gap**: `crypto.aesdecrypt` bad-tag~~ — **A18 was wrong; do not re-add.** The AEAD integrity
  contract is already well covered in `test_crypto.cpp`: a `tamperThrows` helper asserts a flipped
  ciphertext bit, a flipped tag bit, and a wrong AAD each throw, and the 400-round randomized AES-GCM
  fuzz asserts a corrupted tag never decrypts. Remaining real gap here: `int.modinv` non-coprime
  (`"not invertible"`).
- **MED**: crypto RSA/EC verify-fail + wrong-key, `x509parse` garbage PEM, `int.fromstring` bad-digit,
  tensor `einsum`/`tensordot`/`searchsorted` bad-axis.
- Per-type: List `sort`/`apply` callback that `clear()`s the list; `d.update(d)`; Set-algebra with the
  receiver as its own arg; cyclic list into `sorted()`; C++ `Dict::set`/`Set::add`/`List::set`/`setAttr`
  crossing a minor-GC boundary (only `List::push` is pinned); the `gcThreshold` worker-propagation path;
  two-VM-one-thread barrier (A05-3); serde eager-class-var-helper + a `.ki` fresh-VM parity harness.

## Perf — variance analysis (A18 Job 2), the user's key ask

The generational GC did **not** fix per-operation timing variance (intra-run per-rep CV still 0.5–3.7 on
alloc-heavy loops). Root cause: **one threshold governs BOTH minor and major cadence**, so the nursery
holds 50k–120k objects and a *minor* costs ~2 ms — nearly a *major*'s ~4 ms → **bimodal pauses**, not the
generational ideal. A premature-promotion ratchet (`kGcOldAge=1` + un-capped `live*4`) inflates `live` →
inflates the threshold → inflates the arena. (v1.14's "variance fine 2–4%" measured whole-run wall CV,
which averages the spikes out; the per-op jitter the user perceives was never measured.)

### DONE — S3 landed, S1 rejected on measurement

- **S2 (earlier batch)**: reserve the arena vectors upfront. Zero risk, kept.
- **S3 (LANDED)**: minors now run on their own small FIXED nursery (`kMinorNursery = 32768`), majors on
  the live-proportional cadence, counted independently. `setGcThreshold(n)` pins both (so
  `setGcThreshold(1)` still collects on every allocation, and the soak still works).
  Measured against 1.15.0 (`tools/tests/bench`, per-rep intra-run CV, release, reproduced twice):

  | workload | throughput | intra CV | arena high-water |
  |---|---|---|---|
  | sum_loop | +0.4% (flat) | **0.265 → 0.055** | 26.0M → 4.0M |
  | dict_ops | −0.5% (flat) | **0.509 → 0.374** | 100.5k → 43.8k |
  | sort | **25% faster** | 0.019 → 0.014 | 720k → 424k |
  | string_ops | **25% faster** | 0.589 → 0.410 | 165k → 103k |

  No workload regressed. 32768 is the measured knee: 16384 steadies dict_ops further but costs
  sum_loop 22%, because **a minor rescans every remembered old container IN FULL** and a growing List
  re-remembers itself on each append — so halving the nursery doubles the rescan of the same list.
  Card marking (or per-object dirty ranges) is the real answer to that and is the natural next perf
  item; it is what would let the nursery go smaller still.
- **S1 (REJECTED — the measurement contradicted the proposal)**: capping the adaptive threshold is
  harmful. `kGcThresholdFloor` had to go UP (65536 → 131072), not down: the floor must stay a
  comfortable multiple of the nursery or the major trigger fires first every time and **minors never
  run at all** (measured: floor ≤ nursery turns dict_ops major-only, CV 0.785 — worse than 1.15.0).
  And an upper cap would defeat the live-proportional spacing that amortizes a major's O(live) cost —
  the one way to make this quadratic on a genuinely large heap. Spacing majors 8× further apart (the
  pre-split cadence) also measured WORSE everywhere (sum_loop 623M vs 483M, arena 18M vs 4M): with
  promote-on-first-survival, rarer majors grow the arena, and a big arena costs more to sweep than the
  skipped majors save.
- **S4** (`kGcOldAge=2`): still rejected — breaks the `resetRemembered` wholesale-clear invariant
  (guarded by the A05-2 static_assert); would require rescan-retain of old→still-young edges.

**The soak found more than perf:** running `--gc-threshold 1` over the *numeric* suites to validate S3
is what surfaced **A19-1** (nine dangling-handle sites) — see the top of this file.

## False positives / by-design (confirmed, re-checked — do NOT "fix")

Re-verified against `.audit/README.md`'s table and confirmed still correct-by-design: byte-based lexer
columns (A01-4), class-body-var COW aliasing (A09-1), `not <instance>` raw result, exact NaN `==`,
Complex unhashable, left-scalar tensor throw, tensor engine shared by matrix/complex (DRY confirmed).
A mutating `_hash_` breaking Dict/Set self-lookup (A09-4) is an inherent hash-contract violation, not
fixable at the interpreter level (pin a test only).
