# A17 — regex engine + parallel/dispatcher (v1.15.1)

## Scope
- `src/kirito/regex_engine.hpp` (731 L) — recursive-descent parser → bytecode → Thompson-NFA Pike VM
- `src/kirito/stdlib_regex.hpp` (485 L) — the Kirito-facing `regex` module
- `src/kirito/dispatcher.hpp` (787 L) — `KiritoDispatcher`, worker VMs, cross-VM primitives
- `src/kirito/stdlib_parallel.hpp` (600 L) — the Kirito-facing `parallel` module

Contracts under test:
- **regex: LINEAR TIME.** Thompson NFA / Pike VM, no backtracking. A hang or superlinear blow-up IS a bug.
- **parallel: DEADLOCK-SAFE by construction.** `shutdown()` aborts every blocked primitive before joining.

Carried forward from v1.15 (do NOT "fix"):
- A14-1 `(a*)*` nullable-group capture differs from Python — accepted linear-time tradeoff, PINNED.
- A17-2 abort outranks a buffered Queue item; `close()` still drains — deliberate.
- `.audit/README.md` false positives: `Semaphore.release` over-release, `Lock.release` by non-owner — by design.

Probing with `./build-debug/ki`. Every parallel probe wrapped in `timeout`.

---

## Findings

(appended as confirmed)

### A17-3: empty-match advance drops real matches (no Python "must_advance" after a zero-width match)  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/stdlib_regex.hpp:150-164` (`redetail::allMatches`, the `pos = (b > a) ? b : a + 1` step)
- What: After a zero-width (empty) match at position `p`, `allMatches` unconditionally advances to `p+1`. It never retries a NON-empty match starting at the same `p`. Python (and RE2/PCRE `findall`/`finditer`/`sub`/`split`) set a `must_advance` flag after an empty match: at the same position the zero-width match is rejected and a longer match is tried before advancing. So for any pattern whose HIGHEST-priority match at a position is zero-width but which can ALSO match non-empty there (`|\w`, `a||b`, lazy `a??` in a findall context), Kirito silently DROPS the non-empty matches. Affects `findall`, `finditer`, `sub`, and `split` (all route through `allMatches`). This is not a linear-time tradeoff — must_advance is O(1) extra work per position and preserves linearity (RE2 implements it).
- Repro:
  ```
  # regex.findall("|\\w", "ab")  -> ki: ['', '', '']            py: ['', 'a', '', 'b', '']
  # regex.findall("a||b", "ab")  -> ki: ['a', '', '']           py: ['a', '', 'b', '']
  # regex.findall("a??", "aa")   -> ki: ['', '', '']            py: ['', 'a', '', 'a', '']
  # regex.sub("|\\w", "X", "ab") -> ki: 'XaXbX'                 py: 'XXXXX'
  # regex.split("|\\w", "ab")    -> ki: ['', 'a', 'b', '']      py: ['', '', '', '', '', '']
  ```
  Verified with `./build-debug/ki` vs `python3 -c 'import re'` (real output pasted above).
- Impact: any user code doing findall/finditer/sub/split with a pattern that has a higher-priority zero-width alternative (empty branch first, or a lazy `??`) gets fewer matches than every mainstream engine — a silent-wrong-result, not a crash. Discovered by a 21k-case findall differential fuzz against Python (114 divergences, ALL of this shape; 0 divergences where the top match was non-empty; 0 cases where ki accepts a pattern py rejects).
- Proposed fix: in `allMatches`, when the last match was empty (`b == a`), retry `run(p, /*anchored=*/true, requireEnd=false)` requiring a non-empty match at `p` (a variant that rejects a zero-width result), and if found emit it before advancing to `p+1`; else advance. Equivalent to Python's must_advance. RISK: this contradicts the pinned assertion `tools/tests/scripts/r8_net_regex.ki:207` (`re.findall("a||b","ab") == ["a","",""]`) — but that test codifies the divergence itself (its comment "empty alternation branch is a valid empty match" does not acknowledge that Python returns `['a','','b','']`), so the test would be updated to the Python-conformant result as part of the fix. Not in the `.audit/README.md` false-positive table, so not a pinned design decision — distinct from the PINNED `(a*)*` *capture* difference (A14-1), which genuinely is a linear-time tradeoff.
- Proposed test: `tools/tests/scripts/` — assert `regex.findall("|\\w","ab") == ["","a","","b",""]`, `regex.sub("a??","-","aa")` matches Python, and `regex.split("|\\w","ab")` matches Python; plus a C++ case in `test_regex.cpp`.

### A17-4: RETRACTED — negative `count`/`maxsplit` = "replace/split all" is intentional and pinned
- Initially flagged as a Python divergence (Python treats negative count/maxsplit as 0 = do-nothing; Kirito treats it as "all"). On checking the tests this is a DELIBERATE, test-pinned design choice, NOT a bug: `labx_regex.ki:69` (`re.sub("a","X","aaaa",-1)=="XXXX"`, comment "sub negative count = replace all"), `labx_regex.ki:75` ("split negative maxsplit = split all"), `r4_compress_regex.ki:281`, `r7_net_regex.ki:298`. Kirito's "negative == all" is arguably more intuitive than Python's "negative == none". Left as-is. (My initial "no test pins this" note was wrong — I grepped after writing.)

---

## DRY / structural observations (low value, not defects)
- `stdlib_regex.hpp:459-477` — `oneShot` and `oneShotExtra` are near-duplicate module-function delegators (compile → getAttr → call); the only difference is which optional arg carries `flags`. Two copies, so still under the "3+" rule, noted only.
- `stdlib_parallel.hpp` — the `_getstate_`/`_setstate_` pair is copy-pasted across **5** primitive classes (Queue/Lock/Event/Semaphore/Barrier), each ~10 lines differing only in the typed `d.*ById` accessor and the shared_ptr member. A fix to the id round-trip (e.g. the `static_cast<int64_t>`/`static_cast<uint64_t>` narrowing) would have to be applied 5×. A `NativeClass` mixin templated on the accessor would DRY it. Borderline finding; flagged as an observation because each is a distinct type with its own `shared_ptr<T>`.

## Coverage gaps (most valuable first)
- **Empty-match must_advance (A17-3) has NO conformance test** — and worse, `r8_net_regex.ki:207` PINS the divergent output. No `.ki` or C++ test asserts `findall`/`finditer`/`sub`/`split` behavior for a pattern with a higher-priority zero-width alternative (`|\w`, `a??`, `a||b`) against Python. This is the gap that let A17-3 survive.
- **Differential/property testing vs a reference engine is absent** — the existing `test_regex_corpus.cpp` + `spec_regex_corpus.ki` are hand-curated case lists, not a randomized differential against Python `re`. A 32k-case search fuzz + 21k-case findall fuzz (this round) found A17-3; that harness isn't in-tree. Adding a small differential fuzz (offline-generated `cases.json`, run under ki, diff vs a checked-in Python baseline) would catch future empty-match/priority regressions.
- **No test for `sub`/`split` negative-count edge with an empty-capable pattern combined** (the intersection of A17-3 and the pinned negative-count behavior).
- **Barrier concurrent-timeout breakage** (`dispatcher.hpp:281-291`, the `brokenGen_` guard for simultaneous timeouts in one generation) is subtle concurrency logic; grep shows barrier timeout tests exist but a targeted "N workers, some time out mid-generation, barrier stays reusable after reset" race test would harden it. Lives in `test_parallel_sync.cpp`/`test_parallel_deadlock.cpp`.
- **`spawn` thread-exhaustion path** (`dispatcher.hpp:587`, `std::system_error` → clean `KiritoError`) is unreachable in normal tests; no fault-injection coverage (acceptable — hard to test portably).

## Non-findings (probed, correct — do not re-probe)
- **Linear time holds** on every pathological pattern: `(a+)+b`, `(a*)*`, `(a|a)*b`, nested alternations — all sub-3ms on 30-1000 char inputs. The complexity budget (`kMaxMatchWork=1e9`) throws a clean catchable RegexError (~200ms) on `(a?)×800` over 50 KB rather than hanging. Group cap (>1000) and compiled-size cap (200000 insts) and nesting-depth cap (2000) all throw cleanly. `a{100000}` → "repetition count too large (max 1000)".
- **Malformed-pattern rejection is clean** (not a crash) for: `(`, `)`, `[`, `a{2,1}`, `*`, `(?=a)`/`(?<=a)` (lookaround), `a\1`/`(?P=x)` (backrefs), `[z-a]`, trailing `\`, `(?P<1>a)` (bad name), duplicate group name. **No pattern was found where ki accepts something Python rejects** (0/32000 in the fuzz) — so no silent-wrong-match from over-lenient compilation.
- **Match conformance vs Python is exact**: 0 divergences in 32k search cases where both engines compiled the pattern (group text + span). Astral-plane chars (`😀`), astral class ranges (`[\U0001F600-\U0001F64F]`), unicode ignorecase (`À`/`à`), negated classes over astral input, `\Z`/`\z` vs `$`-before-newline, DOTALL, MULTILINE — all correct.
- **sub templates**: `\g<0>` whole-match, `\2\1` reorder, `\g<name>`, out-of-range `\5` → clean "invalid group reference", callable repl (incl. under `--gc-threshold 1` with an allocating callable), callable returning non-String → clean error.
- **pos/endpos**: clamped, None-safe (skipped leading optional via keyword), `endpos` truncates so `$`/`\b` anchor there; negative/huge values clamp without misbehaving.
- **Keyword args** work on every regex function and Match method (in-order, out-of-order, `index=`/`group=`/`pos=`/`endpos=`/`count=`/`maxsplit=`/`flags=`); unknown keyword → clean "unexpected keyword argument". Type errors (`compile(5)`, int subject, Float group key, int repl) all clean.
- **parallel deadlock-safety CONFIRMED**: workers blocked on `Queue.get`/`Event.wait`/`Semaphore.acquire`/`Barrier.wait` with no releaser all unwind on dispatcher shutdown (process exits 0, never 124/hang) — tested individually and all-at-once.
- **parallel primitives**: Queue put/get timeout (full/empty), close (put/get on closed → clean error), negative timeout rejected (`>= 0` check), Lock double-release → clean error, Semaphore/Event/Barrier timeouts, cross-VM Queue transfer, spawn with kwargs, multi-worker Barrier rendezvous, nested-in-function spawn → clean "name 'X' is not defined" (locals-don't-cross), spawn a thrower → catchable "worker thrown: ...", non-serializable arg AND result → clean "cannot dump type" (arg-side before spawn, result-side via worker error), Task.join idempotent + cached, Task.done after error → True. Constructor validation: `Queue(-1)`/`Semaphore(-1)`/`Barrier(0)` reject, `Barrier()` missing-arg, `spawn(5)`/`spawn()` reject. 8-task parallel soak under `--gc-threshold 1` clean.
- **A17-4 (negative count/maxsplit)**: intentional & test-pinned "replace/split all" — NOT a bug (see retraction above).

Status: DONE
