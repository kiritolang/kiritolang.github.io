# A13 — serialization (serialize / dump / json / serde core)

Round: v1.15.1
Scope: `src/kirito/stdlib_serde.hpp` (shared flatten/rebuild core), `stdlib_serialize.hpp` (text KSER1),
`stdlib_dump.hpp` (binary), `stdlib_json.hpp`.
Binary probed: `./build-debug/ki` (no builds run — per round rule).

Status: IN PROGRESS

---

## Log

### A13-1: A10-4 confirmed still open — eager class-var initializer calling a helper that instantiates another class fails in a fresh VM  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/stdlib_serde.hpp:453` (`eagerDepsReady`) + `:466` (`bindEagerHelpers`)
- What: `eagerDepsReady(i)` only inspects class `i`'s OWN direct eager links for `Tag::Class`. When the
  eager initializer calls a captured **Function** helper that instantiates another class, that class is
  not a direct link of `i`, so `i` looks "ready" and is built first. `bindEagerHelpers` then binds the
  helper's free vars, but the not-yet-built class has `objs[id].slot == 0`, so the `if (v.slot)` guard
  skips it and the helper's `Beta` stays a **None placeholder**. The initializer runs and calls None.
- Repro (two processes — the fresh-VM contract; a same-VM load reconnects and hides it):
```
# /tmp/a13/a10_4_save.ki
var io = import("io"); var dump = import("dump")
class Beta:
    var _init_ = Function(self):
        self.v = 777
var mk = Function(): return Beta()
class Alpha:
    var b = mk()
io.print("saved alpha.b.v =", Alpha().b.v)
dump.save(Alpha(), "/tmp/a13/a10_4.kdmp")
# /tmp/a13/a10_4_load.ki
var io = import("io"); var dump = import("dump")
io.print("loaded:", dump.load("/tmp/a13/a10_4.kdmp").b.v)
```
Real output:
```
saved alpha.b.v = 777
--- fresh VM load ---
Traceback (most recent call last):
  File "a10_4_load.ki", line 3, in <module>
a10_4_load.ki:3:18: error: cannot deserialize class 'Alpha': type 'None' is not callable
```
- Impact: any `dump`/`serialize` blob whose class uses a factory-helper class-variable fails to load in
  a fresh VM (the documented headline contract: "deserialization needs no import of the defining module").
- Proposed fix: see the **A10-4 design** section below.

#### A10-4 scope (probed, fresh-VM two-process)
| shape | fresh-VM load |
|---|---|
| `class Derived(Base)` — base is a **direct** eager free var | **OK** (`eagerDepsReady` sees the Class link) |
| `var mk = Function(): return Beta()` / `class Alpha: var b = mk()` | **FAILS** |
| `var mks = [Function(): return Beta()]` / `var b = mks[0]()` (helper via container) | **FAILS** |
| `inner = Function(): return Gamma()` / `outer = Function(): return inner()` / `var g = outer()` (2-level chain) | **FAILS** |

All three failures: `cannot deserialize class 'Alpha': type 'None' is not callable`.
So the hole is exactly: **a class reachable only THROUGH a captured helper's own free variables.**

#### A10-4 proposed design: a TIERED build order (no blob-format change, no double-run)

The v1.15 note frames it as a dilemma — reachability ordering "invents cycles" (pinned by
`test_vm_serialization.cpp:135`, the two classes that only *bind* helpers naming each other), while the
direct rule can't see through a helper. But the two rules do not have to be exclusive. Reachability is a
**sound over-approximation of the dependency**, so use it as a *preference* and fall back:

```
while (remaining):
    # Tier 1 — precise when the graph is acyclic under reachability:
    #   ready(i) := every Class in eagerFrontier(i) is built
    build every tier-1-ready class          # fixes A10-4
    if progressed: continue
    # Tier 2 — reachability reported a (possibly spurious) cycle. Relax to what the body NAMES:
    #   ready(i) := every Class in i's own DIRECT eager links is built     [today's eagerDepsReady]
    build ONE tier-2-ready class, then retry tier 1   # preserves the pinned mutual-bind case
    if progressed: continue
    throw "cyclic class-definition dependency"        # genuinely stuck (corrupt blob)
```

`eagerFrontier` already exists (it drives `bindEagerHelpers`) and already terminates capture cycles via
its `seen` set, so tier 1 is ~5 lines: reuse it, test `nodes[id].tag == Tag::Class && !built[id]`.

Why this is strictly better than both alternatives in the v1.15 note:
- **No blob-format addition.** Recording "eagerly-called" free vars at serialize time needs a new
  `locals.hpp` analysis *and* a wire-format field, and old blobs would still be wrong. It is also still
  only an approximation (`var b = mk` then `b()`, `xs[0]()`, a call through a Dict — none are in call
  position syntactically).
- **No retry / no double-run.** The demand-driven "attempt, defer on failure, retry" design has to
  re-run a class body that already had side effects (a `print`, an `_init_` counter, a module mutation)
  and may already have registered the class. This design never runs a body twice — it only *chooses an
  order*.
- **Cannot regress the pinned case.** When reachability is cyclic, tier 2 reproduces today's behaviour
  exactly. Traced by hand:
  - A10-4: `eagerFrontier(Alpha) = {mk, Beta}` → Alpha blocked; `eagerFrontier(Beta) = {}` → Beta built
    first; then Alpha builds and `bindEagerHelpers` binds `mk`'s `Beta` to the **real** class. Fixed.
  - Pinned mutual-bind: `eagerFrontier(Alpha) ∋ Beta`, `eagerFrontier(Beta) ∋ Alpha` → tier 1 stuck →
    tier 2 builds Alpha (its direct eager links hold only the Function `makeBeta`) → tier 1 then builds
    Beta. Same outcome as today; `makeBeta`'s `Beta` is patched by pass 5, and Alpha only *binds* it.
  - Self-referential (`class Node: var proto = mkNode()` where `mkNode` returns `Node()`): frontier
    contains itself → tier 2 → builds → fails at runtime exactly as it does today. No regression.
- Proposed test: `test_vm_serialization.cpp`, beside the existing empty-prelude eager cases —
  the three shapes in the table above (helper factory, helper-via-container, 2-level chain), each with
  an **empty prelude** so the class is genuinely rebuilt. All three fail on the unfixed build with
  "type 'None' is not callable". Keep the mutual-bind case as the anti-regression pin.

## Non-finding: the full value-type matrix round-trips in a FRESH VM, both codecs, under `--gc-threshold 1`

`/tmp/a13/types_save.ki` builds a 30-key Dict covering every documented type and `/tmp/a13/types_load.ki`
asserts each **value** in a **second `ki` process** (no defining module imported). Both codecs, plain and
`--gc-threshold 1`. All values outside the small-int intern range where applicable (1000/2000/4242/9999).

Covered + verified: `None`/`True`/`False`; Integer `123456789012345`, `int64` min+1 / max, `4242`,
`-4242`; Float `0.1`, `-0.0` (sign preserved), `1e400`→inf, `5e-324` (denormal min); String with
U+00E9 + U+1F600 (astral) + empty; `Bytes([0,255,128,7])` (incl. NUL + high bytes); nested List, empty
List; Set (membership + len); Dict with mixed String/Integer keys; user instance (attrs + a **method
call** on the loaded object); **shared identity** (`id(a)==id(b)` across two Dict slots); `_getstate_`/
`_setstate_` (private `_n` restored); a **self-referential cycle** (`id(cyc[1])==id(cyc)`);
Matrix (element + determinant), Vector (norm), Complex (re/im), ComplexMatrix (element),
DateTime (`.iso()`), Random (**exact stream** — the reloaded RNG's next `randint` equals a
fresh `Random(4242)`'s), Tensor (`tolist()`); a Function value with a **default arg**, called
positionally AND by keyword after the load; a class VALUE, instantiated after the load.

```
### dump, plain            -> only matrix_det (see below)
### dump, --gc-threshold 1 -> same
### serialize, plain       -> same
### serialize, --gc-threshold 1 -> same
```
The single `FAIL matrix_det got -2.0 want -2.0` is **my test's** fault, not serde's: the determinant is
`-2.00000000000000088818` in the **live** VM too (Gaussian-elimination rounding; display shows `-2.0`).
Verified live: `live det == -2.0 exact -2.0? False repr -2.00000000000000088818`. The blob round-trips
the exact bits — that is the point.

**No GC/write-barrier finding in the rebuild path** across this matrix (A19-2's neighbourhood).

## Non-finding: binary `dump` codec survives systematic mutation (3896 mutants, 0 escapes)

Harness `/tmp/a13/mutate.py` over a 437-byte blob containing every node tag (Integer/String/List/Set/
Object/Class/Function/Module + a cycle). Each mutant is loaded in a **fresh `ki` process** inside
`try/catch`; a mutant is a finding if the process exits non-zero (crash/uncaught) or hangs (25s timeout).
- **437** truncations (every prefix length 0..436)
- **3059** single-byte mutations (every offset x {0x00,0x01,0x7f,0x80,0xff,0xfe,0x0a})
- **400** random 1-6-byte corruptions (seed 13)

```
distinct bad classes: 0
```
Every mutant either loaded or threw a clean **catchable** Kirito error. No crash, no hang, no raw C++
exception escaping. This re-confirms v1.15's A10-3 fix (the `std::exception`→`KiritoError` translation in
`dumpfmt::read`) and the decode-side guards.

Static re-check of the remaining untrusted-integer paths in `dumpfmt::decode` + `serde::rebuild`
(reasoned, not fuzzed — the fuzz found no way to reach them):
- `objectCount` — guarded (`n > data.size()`), and `reserve` is capped at 1<<16 so a huge count cannot
  amplify the allocation.
- huge String/`bytes(n)` length — `need()` compares `pos_ + n > s_.size()` in `size_t`; a `uint32` `n`
  cannot overflow that on 64-bit.
- huge List/Dict/Set/Function/Class link counts — the loop calls `r.u32()` per link, which throws the
  moment the stream runs short, so the count can never outrun the data.
- `Node::i` (Class eager count) — read as `u32`, so never negative; **every** consumer that indexes with
  it (`eagerCountOf` → `eagerFrontier::pushLinks`, `eagerDepsReady`) clamps to `links.size()/2`.
  `declareFreeVars` casts `nd.i` unclamped but only uses it for the `k < eagerCount` **flag** — its loop
  bound is `pairs`, so it cannot over-index. Correct, though the asymmetry is a latent trap: see A13-2.
- `links` is always even-length (decode reads `c*2`), and pass 5 / the wiring loops all use
  `k + 1 < size` or `size/2` bounds, so an odd count could not over-index either.

### A13-2: a class whose eager class-var initializer reads a captured INSTANCE / native value fails to load with a raw `dangling handle (stale generation)`  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/stdlib_serde.hpp:305-313` (`declareFreeVars` — no `.slot` guard), vs `:466-477`
  (`bindEagerHelpers` — **has** `if (v.slot)`). Both codecs (shared core).
- What: two defects, one visible symptom.
  1. **The round-trip hole.** `rebuild` hard-orders **classes (pass 0c) before instances (pass 1)**, and
     `eagerDepsReady` only waits on `Tag::Class` deps. So a class whose eager class-var initializer
     reads a captured **instance** (`Tag::Object`) or a captured **native value** (`Tag::Stateful` —
     Matrix/Complex/DateTime/Random/Tensor) can never see it built: `objs[id]` is still a
     default-constructed `Handle{}`. The graph is perfectly well-founded (Cfg class → cfg instance → K
     class is a valid order), but the pass structure cannot express it.
  2. **The DRY defect** (this is the one that makes the message awful). `bindEagerHelpers` guards
     `if (v.slot)` before defining; `declareFreeVars` — the *other* copy of the same
     "bind a free var that may not be built yet" logic — does **not**, and defines the null
     `Handle{}` straight into the env. The `.slot` guard was added to one copy and not the other.
- Repro (fresh-VM, two processes):
```
# save
var dump = import("dump")
class Cfg:
    var _init_ = Function(self):
        self.field = 4242
var cfg = Cfg()
class K:
    var v = cfg.field
dump.save([K(), cfg], "/tmp/a13/eagerinst.kdmp")   # prints "saved 4242" live
# load, fresh VM
var o = dump.load(arglist[0])
```
Real output (identical under `--gc-threshold 1`, and identical for the **text** `serialize` codec):
```
load.ki:3:18: error: cannot deserialize class 'K': dangling handle (stale generation)
```
Same for a captured **native** value (`var m = M.Matrix(...)` / `class K: var v = m[0, 0]`):
```
=== eagermat (dump, fresh) ===
load.ki:3:18: error: cannot deserialize class 'K': dangling handle (stale generation)
```
The **helper-mediated** form goes through the guarded copy and so degrades gracefully instead:
```
=== eagerhelperinst (dump, fresh) ===   # var get = Function(): return cfg.field ; class K: var v = get()
load.ki:3:18: error: cannot deserialize class 'K': type 'None' has no attribute 'field'
```
That contrast is the proof that the missing `.slot` guard, not the ordering, produces the bad message.
- **Not memory-unsafe** — I checked before reporting: `arena.hpp:167` reserves generation 0 for the
  `Handle{}` sentinel ("never 0 while live"), so `at()`'s `slot.generation != h.generation` check
  **always** fires for a null handle. The sentinel design contains it. This is a robustness/diagnostic
  + round-trip finding, not a UAF.
- Impact: (a) a user sees an internal memory-safety diagnostic for a valid program — it reads like an
  interpreter bug and is unactionable; (b) the documented self-contained fresh-VM round-trip does not
  hold for a class with an instance/native-valued class variable.
- Proposed fix, in two independent steps:
  1. **Now (small, safe):** give `declareFreeVars` the same `.slot` guard `bindEagerHelpers` has —
     `Handle val = (eagerReal && eager && objs[id].slot) ? objs[id] : vm.none();`. Better: single-source
     the two copies into one `bindFreeVar(env, name, handle)` helper so the guard cannot go missing from
     one of them again. Turns the internal diagnostic into the ordinary
     `type 'None' has no attribute 'field'` that the helper path already gives. Breaks no contract —
     no valid blob currently reaches the unguarded path successfully.
  2. **Properly:** let pass 0c build an instance/native-stateful on demand when a class's eager frontier
     needs it. This composes with the A13-1 (A10-4) tiered order: extend the tier-1 readiness test from
     "every **Class** in `eagerFrontier(i)` is built" to "every **Class, Object or Stateful** in
     `eagerFrontier(i)` is built", and let the pass-1 instance/stateful construction be callable for a
     single id from pass 0c (it only needs `vm.findClass(nd.s)` / the registered factory, both available
     once the member's own class is built). Tier 2's fallback keeps every currently-loadable blob
     loading. Worth confirming the attribute-wiring order (pass 2) still precedes any read — for a
     *read* of `cfg.field` the instance must be wired too, so this step must hoist that member's pass-2
     wiring as well; if that proves too invasive for a patch release, ship step 1 alone and document the
     limitation.
- Proposed test: `test_vm_serialization.cpp`, beside the empty-prelude eager cases. Empty prelude,
  `class K: var v = cfg.field` with a captured instance, plus the Matrix variant. Asserts the restored
  `K().v == 4242`. Fails today with `dangling handle (stale generation)`. If only step 1 ships, pin the
  **message** instead (must not mention "dangling handle") so the internal diagnostic cannot return.

## Non-finding: text `serialize` (KSER1) codec survives systematic + crafted mutation (~4300 mutants, 0 escapes)

Same harness shape (`/tmp/a13/mutate_ser.py`) over a 323-byte KSER1 blob with every tag:
- **323** truncations; **~3553** single-byte mutations (every offset x the tokenizer-relevant alphabet
  `0-9`, space, `X`, `K`,`S`,`E`,`R`,`1`, `0x00`, `0xff`); **400** random 1-6-byte corruptions (seed 7);
- **28 hand-crafted hostile blobs** aimed at the text codec's `stol`/`stoll`/`stoi` paths specifically
  (this codec is a *separate* hand-written parser from the binary one, so it needed its own pass):
  out-of-int64 counts, negative counts, `KSER1 -1`, huge object counts, huge/negative string lengths,
  huge/garbage ids, non-numeric `B`/`I`/`F` payloads, empty/partial headers.
```
distinct bad classes: 0
```

**Targeted probe — the one real asymmetry between the codecs.** The binary codec reads the Class eager
count as `u32` (never negative); the text codec's `case 'C'` uses a bare `std::stoll(token())` with **no
`countToken()` range check**, so it is the only way to get a negative/huge `Node::i` into `rebuild`. I
surgically rewrote the eager-count field of a *valid* blob for a class that really has one eager free
variable (`var factor = 3000; var helper = ...; class W: var s = helper(10)`):
```
class record eager,pairs = b'1' b'1'
'-1'                    rc= 0 OK        # (size_t)-1 -> SIZE_MAX -> eagerCountOf clamps to pairs
'-9223372036854775807'  rc= 0 OK
'999'                   rc= 0 OK        # clamped to pairs
'0'                     rc= 0 CAUGHT    # helper stays a None placeholder -> clean catchable error
'4294967297'            rc= 0 OK        # clamped
```
So the missing range check is **not** exploitable: every indexing consumer clamps, and
`declareFreeVars`'s unclamped cast only drives a flag whose loop bound is `pairs`. Worth tightening for
symmetry/defence-in-depth (`case 'C'` should use `countToken()`-style validation like every other count
in the same function), but it is a latent trap, not a bug — filing it as such rather than as a finding.

### A13-3: `json.loads` rejects a lone high surrogate followed by a `\u` escape, but U+FFFD-substitutes the identical input spelled literally  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/stdlib_json.hpp:154` (`else fail("invalid low surrogate in \u escape")`)
- What: the code's own stated rule (`stdlib_json.hpp:156-159`) is "an **UNPAIRED** surrogate ... is not a
  valid code point ... substitute U+FFFD (like browsers / WHATWG) so the decoded String is always
  well-formed UTF-8". That rule holds everywhere **except** one branch: when a high surrogate is followed
  by a `\u` escape that is not a low surrogate, the parser `fail`s instead of substituting. The
  speculative lookahead consumed the second escape and then treats a perfectly decodable input as
  malformed. `A` and `A` are the *same character* in JSON, yet they parse differently:
- Repro (`/tmp/a13/surr.ki`), real output:
```
lone high + end       \ud800        -> len 1 cps [65533]     # FFFD  (rule applied)
lone high + plain A   \ud800A       -> len 2 cps [65533, 65]  # FFFD A (rule applied)
lone high + A    \ud800A  -> THROWS                 # <-- same input, other spelling
lone high + \ud800    \ud800\ud800  -> THROWS                 # <-- should be FFFD FFFD
lone low              \udc00        -> len 1 cps [65533]      # FFFD  (rule applied)
lone low + A     \udc00A  -> len 2 cps [65533, 65]  # FFFD A (rule applied)
valid pair            😀  -> len 1 cps [128512]     # correct pairing
```
- Impact: real-world JSON emitted by serializers that pass lone surrogates through (Java/C#/older
  JS `JSON.stringify` of an unpaired surrogate) fails to parse in one spelling and succeeds in the other.
  A caller cannot work around it without pre-editing the text. Low: it is a clean rejection, never
  corruption, and the input is itself already non-conforming.
- Proposed fix: one line — replace `else fail(...)` with `else pos_ -= 6;` (rewind the speculatively
  consumed `\uXXXX`). `cp` then remains the high surrogate, falls into the existing U+FFFD substitution,
  and the main loop re-reads the second escape independently. Result: `\ud800A` -> FFFD + A,
  `\ud800\ud800` -> FFFD + FFFD — exactly matching the literal spellings above and the stated WHATWG
  rule. Genuinely malformed hex is unaffected: `\ud800\uZZZZ` still throws "invalid \u escape" on the
  re-read. Breaks no documented contract (the throw is not in the docs, and nothing pins it — see
  Coverage gaps).
- Proposed test: `tools/tests/scripts/verify_json.ki` (which already covers surrogate pairs) — assert
  the four unpaired-surrogate spellings all yield U+FFFD, and that a valid pair still combines.
  Fails today on the two `\u`-followed cases.
