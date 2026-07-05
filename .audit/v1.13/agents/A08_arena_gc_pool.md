# A08 — Memory: arena, handles, GC, pool, VM, environment

Audit of `src/kirito/arena.hpp`, `pool.hpp`, `vm.hpp`, `handle.hpp`, `environment.hpp`.
Read-only static analysis. Findings below.

## Summary of surface reviewed
- `arena.hpp`: slot table (`unique_ptr`+generation+occupied+marked), free-list reuse, `alloc`/`deref`/`at`, `clearMarks`/`markIfUnmarked`/`sweep` with generation bump + `UINT32_MAX` retirement, `liveCount`/`capacity`. Gen 0 reserved as `Handle{}` sentinel, real gens `[1, UINT32_MAX]`.
- `handle.hpp`: `Handle{slot,gen}`, `NamedArg`, `std::hash<Handle>`.
- `pool.hpp`: thread-local segregated free-list, kAlign=16, kMaxPooled=224, kClasses=14 (16..224), `classOf`, sanitizer bypass, `FreeLists` dtor drains lists at thread exit.
- `vm.hpp`: `KiritoVM::alloc` (GC-before-alloc), interned `none_/true_/false_/undefined_/smallInts_`, `makeInt/Float/String/Bool`, tempRoots/RootScope, pinHandle/unpinHandle refcount map, auxRoots operand-stack regions, bytecodeConsts pin pool, `collectGarbage` root enumeration, adaptive threshold, call-depth+stack-bytes guard, `_activeVM` thread_local.
- `environment.hpp`: `EnvValue` with `SmallVec` (kInline=4) flat bindings, `children()`, `envLookup`/`envAssign`.

Overall: the memory subsystem is carefully engineered; no confirmed UB/crash bug in these five files. Findings are one latent dangling-pointer edge case, a rooting design hazard, and a broad set of coverage gaps around the correctness-critical-but-hard-to-reach paths (generation wraparound, intern boundaries, pool size classes).

---

### A08-1: Out-of-LIFO-order VM destruction leaves `_activeVM` dangling
- severity: low
- location: `vm.hpp:36-37` (ctor), `runtime.hpp:3447-3451` (dtor)
- category: use-after-free (latent)
- description: The ctor does `_prevActiveVM = _activeVM; _activeVM = this;` and the dtor restores only `if (_activeVM == this) _activeVM = _prevActiveVM;`. This is correct only for strict LIFO nesting of VMs on one thread. If VM A is created, then VM B (B._prevActiveVM=A, _activeVM=B), and A is destroyed *before* B (non-LIFO — e.g. heap VMs, containers, `std::optional`), A's dtor sees `_activeVM==B` and does nothing; later B's dtor restores `_activeVM = _prevActiveVM = &A`, which now dangles. Any subsequent user-class `_hash_`/`_eq_` on that thread reads a freed pointer via `activeVM()`.
- failure-scenario: `auto a = std::make_unique<KiritoVM>(); auto b = std::make_unique<KiritoVM>(); a.reset(); /* use b, put a class instance in a Set */ b.reset();` — `_activeVM` briefly points at freed A.
- proposed-test: unit test creating two heap VMs, destroying the outer first, then exercising a `_hash_`-driven Set insert on the survivor under ASan.
- proposed-fix: track the active-VM stack explicitly, or on dtor also clear `_activeVM` if it equals `_prevActiveVM`-that-is-being-destroyed; simplest robust fix: keep a thread_local `std::vector<KiritoVM*>` and pop-by-value.
- confidence: medium (real code path; requires non-LIFO multi-VM-per-thread, which is unusual)

### A08-2: `newScope(parent)` can sweep `parent_` if the parent handle is unrooted across the alloc
- severity: low
- location: `vm.hpp:78` `newScope`; interacts with `alloc` GC-before-alloc at `vm.hpp:58-61`
- category: gc-safety (design hazard / contract)
- description: `std::make_unique<EnvValue>(parent)` constructs the EnvValue (holding `parent_` as a child, see `environment.hpp:33-37`) *before* `alloc()` runs, and `alloc()` may trigger `collectGarbage()` before the new EnvValue is registered as a root. The just-built EnvValue is not yet reachable, so its child `parent` is protected only if the caller has `parent` otherwise rooted. In-tree callers pass `global_` (a permanent root) or a scope on the live operand stack, so it is safe today — but the contract is implicit and a future caller passing an unrooted parent would get a scope with a dangling `parent_`.
- failure-scenario: a C++ embedder builds a detached scope chain: `Handle p = vm.newScope(vm.global()); Handle c = vm.newScope(p);` where `p` is held only in a bare local and the second `newScope`'s GC reclaims `p`.
- proposed-test: with `setGcThreshold(1)`, build a 3-level scope chain via `newScope` holding only bare locals, define a binding in the root, then read it through the chain after churn.
- proposed-fix: `RootScope`-protect `parent` inside `newScope` before the `alloc`, or document the rooting precondition on the method.
- confidence: medium (mechanism is real; no in-tree trigger)

### A08-3: Generation increment / wraparound / slot-retirement path is entirely untested
- severity: medium
- location: `arena.hpp:54-74` (`sweep`, esp. the `generation == UINT32_MAX` retirement at 62-69 and `++s.generation`)
- category: coverage gap (correctness-critical)
- description: The generation-bump-on-free and the ABA-retirement branch guard the core "stale handle can't alias a recycled slot" invariant. No test drives even a single generation increment assertion, and the retirement branch (leak-one-slot at gen `UINT32_MAX`) is unreachable in practice (needs 2^32 reuses) so it is never exercised. A regression here (e.g. forgetting to bump, or reusing a retired slot) would silently reintroduce ABA aliasing.
- failure-scenario: refactor drops `++s.generation`; a freed-then-reused slot re-validates an old handle to a different object — silent data corruption, no test fails.
- proposed-test: unit test directly on `ObjectArena`: alloc → drop root → sweep → alloc again → assert the *old* handle now `CHECK_THROWS(deref)` (stale gen) while the new handle derefs the new object; to reach the retirement branch, add a test-only hook / friend to preset a slot's generation to `UINT32_MAX-1` and verify it is retired (off free-list) after two sweeps.
- proposed-fix: none (add tests); optionally expose a test-only generation setter behind a friend so the retirement branch is coverable.
- confidence: high

### A08-4: Small-int interning boundaries not tested (only `id(1)==id(1)`)
- severity: low
- location: `vm.hpp:69-72` `makeInt`, `kSmallIntLo=-256`/`kSmallIntHi=256` (`vm.hpp:342-343`); existing test `tools/tests/scripts/audit_builtins.ki:770`
- category: coverage gap
- description: Only `id(1) == id(1)` is asserted. The interning boundary is untested: that `id(256)==id(256)` and `id(-256)==id(-256)` (interned) but `id(257)!=id(257)` and `id(-257)!=id(-257)` (fresh alloc each time), and that `smallInts_[v-kSmallIntLo]` indexing is off-by-one-free at both ends. An off-by-one in the range check would go unnoticed.
- failure-scenario: change `>= kSmallIntLo && <= kSmallIntHi` to `<` and 256 stops interning — no test catches it.
- proposed-test: `.ki` or C++ assertions on `id()` identity at 255/256/257 and -256/-257; C++ `CHECK(vm.makeInt(256)==vm.makeInt(256))` and `CHECK(vm.makeInt(257)!=vm.makeInt(257))`.
- confidence: high

### A08-5: Small-object pool size classes / boundary / exact recycling not directly tested
- severity: low
- location: `pool.hpp:57-76` (`classOf`, `allocate`, `deallocate`), boundary `kMaxPooled=224`
- category: coverage gap
- description: `test_gc_stress.cpp` churns 100k boxes but only asserts `liveCount` stays bounded — it never validates the pool's own invariants: that `classOf` maps sizes to the right class at boundaries (16/17/224/225), that a >224-byte object bypasses the pool symmetrically in alloc+dealloc, and that a recycled block is reused for a same-class object (exact-size reuse). A wrong-size-class or double-free bug in the pool would only surface as ASan noise (and ASan *bypasses* the pool), so release-only pool corruption has no gate.
- failure-scenario: `classOf` off-by-one pushes a 224-byte block onto the wrong list; a later same-class alloc hands back a too-small block → heap corruption, invisible to sanitizers (pool disabled there).
- proposed-test: a release-build unit test allocating/freeing objects straddling 16/224/225-byte sizes in interleaved order and asserting no corruption (e.g. write/verify a canary), plus a white-box `classOf` boundary table check.
- proposed-fix: none (add tests).
- confidence: medium

### A08-6: Arena `capacity()` / free-list reuse never asserted
- severity: low
- location: `arena.hpp:24-35` (free-list reuse), `arena.hpp:80` `capacity()`
- category: coverage gap
- description: GC tests assert `liveCount()` stays bounded, but a bounded live set does NOT prove `capacity()` (total slots) is bounded — if `sweep` failed to push freed slots to `free_`, capacity would grow unboundedly while liveCount stayed flat. `capacity()` is never checked in any test.
- failure-scenario: a bug drops `free_.push_back(i)` in sweep; every collection still frees objects (liveCount fine) but slots_ grows forever → slow memory leak, no test fails.
- proposed-test: churn N garbage allocations with a small threshold, then assert `vm.arena().capacity()` stabilizes (does not grow ~linearly with total allocations).
- confidence: high

### A08-7: ABA "stale handle validates against a recycled slot" not explicitly tested
- severity: low
- location: `arena.hpp:46-52` `markIfUnmarked`, `arena.hpp:93-98` `at`
- category: coverage gap
- description: `test_r4_cpp_api.cpp:348-349` tests that a dropped handle dangles after sweep (occupied=false path). But the sharper invariant — a slot is swept, then *reused* by a new alloc (occupied=true, generation bumped), and the OLD handle must still be rejected by generation mismatch — is not exercised. This is the property generations exist for.
- proposed-test: alloc X (handle hX), drop root, `collectGarbage`, alloc Y (likely same slot, new gen), assert `deref(hX)` throws (stale generation) while `deref(hY)` gives Y.
- confidence: high

### A08-8: `EnvValue`/`SmallVec` spill, copy, and deep-chain lookup untested at unit level
- severity: low
- location: `environment.hpp:23-147` (whole file), esp. `SmallVec` spill at `kInline=4` and copy ctor/assign (73-77), `envLookup`/`envAssign` chain walk
- category: coverage gap
- description: No unit test targets `EnvValue` directly. Untested: SmallVec spilling from inline storage to heap (>4 bindings) and back via `clear()` resetting `cap_=kInline`; the copy ctor/assign correctness (used for class-attribute copy-on-write); `envLookup`/`envAssign` innermost-first resolution across a deep chain; `define` overwriting an existing binding vs appending. These run implicitly through scripts but a targeted regression net is absent.
- failure-scenario: a `SmallVec::grow` off-by-one on the inline→heap transition corrupts a scope with exactly 5 bindings — only caught if a script happens to have a 5+ binding scope AND reads all of them.
- proposed-test: C++ unit test building an `EnvValue`, defining >4 names (force spill), reading them, redefining, copying the env, and walking a hand-built parent chain via `envLookup`/`envAssign`.
- confidence: medium

### A08-9: Unbounded monotonic growth of retained chunks / Proto cache / pinned consts in long-running eval loops
- severity: low
- location: `vm.hpp:115` `pinConst`→`bytecodeConsts_`, `vm.hpp:124` `protoPut`→`protoCache_`, `runtime.hpp:3443-3445` `retainChunk`→`chunks_`
- category: weak spot (memory growth)
- description: Every compiled body's literal constants are pinned for the VM's lifetime and every parsed chunk's AST is retained forever (so closures can't dangle). A process that repeatedly `runSource`/`runRepl`/`evalIn`s fresh source (a REPL, a server evaluating user snippets, `eval`-in-a-loop) accumulates `chunks_`, `protoCache_`, and `bytecodeConsts_` monotonically — none are ever released. Correct for closures, but an unbounded leak for long-lived eval-driven services.
- failure-scenario: a REPL or eval-loop server runs for days; RSS climbs without bound though the live value set is tiny.
- proposed-fix: out of scope to fix; document the growth, or offer an opt-in "transient chunk" mode that frees a chunk+its Protos+consts once no closure references it (requires reachability tracking). At minimum note it.
- confidence: medium

### A08-10: No arena slot cap; deep scope chains give O(depth) linear lookup
- severity: low
- location: `arena.hpp` (no cap on `slots_`), `environment.hpp:128-147` `envLookup`/`envAssign`
- category: weak spot
- description: The arena has no upper bound on slot count — a program building a genuinely-live huge structure OOMs rather than throwing a Kirito-level resource error (CLAUDE.md notes guards exist for list/string repetition, range, and nesting depth, but not for total live-object count). Separately, `envLookup` walks the parent chain linearly; a pathologically deep lexical nesting makes every name resolution O(depth). Neither is a bug, but both are unguarded scaling cliffs.
- proposed-fix: optional soft cap on `capacity()` that throws a catchable error; none needed for the lookup (compile-time slot addressing already mitigates the hot path).
- confidence: medium

### A08-11: `Handle` carries no arena/VM identity — cross-arena handle confusion is undetectable
- severity: low
- location: `handle.hpp:19-23`; consumers `arena.hpp:46-52`, `93-98`
- category: robustness (design)
- description: A `Handle` is just `{slot, generation}`. `markIfUnmarked`/`at` validate slot-in-range + generation match, but a handle minted by a *different* VM's arena with a coincidentally matching slot+generation would falsely validate. The share-nothing model means live handles never legitimately cross VMs, so this is not a practical bug — but a stray/misused handle produces silent wrong-object access instead of a clean error, and it cannot be asserted against.
- proposed-fix: (heavy) add an arena-id byte to Handle in debug builds; or accept as documented invariant. Note only.
- confidence: high (design fact)

### A08-12: `std::hash<Handle>` collapses on 32-bit `size_t`
- severity: low
- location: `handle.hpp:31-36`
- category: correctness (portability) / -Wconversion-adjacent
- description: `(static_cast<std::size_t>(h.generation) << 32) ^ h.slot`. On an LP32/ILP32 target where `size_t` is 32-bit, the `<< 32` is undefined/zero, so the hash degenerates to `h.slot` alone — every generation of a given slot hashes identically, degrading `unordered_map<Handle,int>` (pinnedRoots_) and any Handle-keyed set to per-slot collision chains. Kirito targets 64-bit primarily, so low impact, but it is technically UB on 32-bit.
- proposed-fix: guard the shift with `if constexpr (sizeof(std::size_t) >= 8)` or mix via a 32-bit-safe combiner (e.g. `slot * 0x9E3779B1u ^ generation`).
- confidence: high

### A08-13: `arglist_` unset-sentinel uses `.slot==0`, inconsistent with the `replScopeReady_` bool pattern
- severity: low
- location: `vm.hpp:136` (`if (arglist_.slot) enqueue(arglist_)`), `runtime.hpp:3492` (`if (!arglist_.slot)`), vs `replScope_`/`replScopeReady_` (`vm.hpp:329-330`, `collectGarbage` `vm.hpp:133`)
- category: DRY / fragility
- description: Two "is this optional VM handle set?" checks use two different idioms. `replScope_` pairs with an explicit `replScopeReady_` bool (robust). `arglist_` instead tests `arglist_.slot != 0`, which is correct ONLY because `none_` is guaranteed to occupy slot 0 (first arena alloc, `vm.hpp:39`) so no real handle ever has slot 0. This couples an unrelated invariant (none_ is slot 0) into the arglist guard; if singleton allocation order ever changed, `arglist_` could be spuriously treated as set/unset. A `Handle{}`-equality check (`arglist_ == Handle{}`) or a bool flag would be self-evidently correct and match `replScope_`.
- proposed-fix: use `arglist_ != Handle{}` (default sentinel) or an `arglistReady_` bool, mirroring `replScopeReady_`.
- confidence: high

### A08-14: Stale comment in `pool.hpp` `classOf`
- severity: low
- location: `pool.hpp:57`
- category: doc drift
- description: `inline std::size_t classOf(std::size_t n) { ... }  // 1..64 -> 0..3` — the `1..64 -> 0..3` mapping describes an older `kMaxPooled=64`/4-class configuration; the current config is `kMaxPooled=224`, `kClasses=14`, mapping 1..224 to classes 0..13. Misleading to a reader verifying the pool.
- proposed-fix: update the comment to `// 1..224 -> 0..13`.
- confidence: high

### A08-15: `installBuiltins`/`installStandardLibrary` run in the ctor with GC live (threshold 100000)
- severity: low
- location: `vm.hpp:49-50` (ctor calls), `vm.hpp:58-61` (`alloc` may collect)
- category: gc-safety (latent)
- description: The constructor allocates the singletons directly via `arena_.alloc` (no GC — safe), but then calls `installBuiltins()`/`installStandardLibrary()`, whose allocations go through `vm.alloc` and *can* trigger `collectGarbage()` if they exceed `gcThreshold_` (100000). Mid-install collection is safe today because installed values are attached to `global_` (a root) as they are registered and native modules are held in `nativeModules_`/factories — but this relies on every install step rooting its product before the next allocation. It is an untested invariant during a fragile window (the VM is not yet fully constructed; `moduleCache_`/etc. are being populated).
- failure-scenario: a future stdlib module that builds a large intermediate structure in a C++ local before registering it, exceeding the threshold mid-build, gets it swept.
- proposed-test: construct a VM with `setGcThreshold(1)` forced *before* stdlib install is possible — not currently expressible (install is in the ctor); alternatively add an internal hook to lower the threshold during install in a debug build and assert the VM still boots and all builtins resolve.
- proposed-fix: wrap stdlib install in a `GcPauseScope` (already available) inside the ctor, or lower-risk: leave as-is but document. Confidence the current code is correct is high; the concern is future-proofing + testability.
- confidence: medium

