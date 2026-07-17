# A06 — List / Dict / Set / Array (every method, every argument)

Round: v1.15.1. Scope: `src/kirito/collections.hpp` + the List/Dict/Set type-method surface in
`runtime.hpp`; plus the `.ki`/C++ test coverage for that surface.

Rules honoured: **no builds** (probe `./build-debug/ki` only); every probe also under
`--gc-threshold 1`; values outside the small-int intern range (>256 / <-256).

Known false positives for my surface (from `.audit/README.md`, NOT to be re-litigated):
- Dict/Set iteration order is not insertion-stable across delete — by design (unordered).
- A NaN Set element / Dict key is write-only — by design (exact IEEE-754 `==`).

## Log

(appending as I go)

### A06-1: `Dict.popitem()` SEGFAULTS on a Dict containing a NaN key  [severity: HIGH] [confidence: confirmed]
- Location: `src/kirito/runtime.hpp:837-850` (`DictVal::getAttr`, `popitem`), specifically line 844:
  `Handle v = rs.add(*d.find(vm.arena(), k));`
- What: `popitem` takes `k = keys().back()` — a key that is definitely PRESENT in the bucket — and then
  re-looks it up with `find()` to get its value. `find()` locates a key by `probeBucket`, i.e. by
  `equals()`. For a **NaN** key `equals()` is never true (exact IEEE-754, `NaN != NaN`), so `find()`
  returns **`nullptr`** and the `*` dereferences it → null-pointer deref, SIGSEGV. The whole
  interpreter dies; the error is not catchable by a Kirito `try`.
  This is NOT the documented "NaN keys are write-only" false positive — write-only means *insertable
  but not findable*, and every other Dict op honours that contract correctly (`get`/`pop`/`remove`/
  `d[k]` all check `find()`'s result and report a clean miss; `str`/`keys`/`len` all work). `popitem`
  is the single site that dereferences `find()` **unchecked**. Same precedent as the v1.13 A25-1/A25-2
  tabular NaN-key crashes: the core NaN semantics are intentional, a **crash** is still a bug.
- Repro (`/tmp/p1.ki`):
  ```kirito
  var io = import("io")
  var big = 1e308 * 10.0
  var nan = big - big
  io.print("nan is", nan)
  var d = {}
  d[nan] = 900
  io.print("len", len(d))
  io.print("keys", d.keys())
  var r = d.popitem()
  io.print("popitem", r)
  ```
  REAL output:
  ```
  nan is nan
  len 1
  keys [nan]
  timeout: the monitored command dumped core
  /bin/bash: line 25: 339602 Segmentation fault      timeout 20 .../build-debug/ki p1.ki
  EXIT=139
  ```
  Also crashes in the realistic **drain loop** (`/tmp/p5.ki`) — a dict with a normal key AND a NaN key,
  `while len(d) > 0: d.popitem()`:
  ```
  timeout: the monitored command dumped core
  339984 Segmentation fault
  EXIT_LOOP=139
  ```
- Impact: any program that drains a Dict with `popitem()` where a NaN ever reached a key position —
  e.g. a Dict keyed by computed floats (a stats/tabular bucket key, a parsed `Float("nan")`, `0.0*inf`).
  Hard crash of the host process; in an **embedded** VM it takes the host application down, and in a
  `parallel` worker it kills the whole process. Not catchable.
- Proposed fix: `popitem` already has the value in hand without re-finding — walk the bucket directly
  (like `popArbitrary` does for `SetVal`, which is why `Set.pop` on a NaN member is **safe**, verified
  below). Cleanest: give `DictVal` a `popArbitrary()` returning the `{key, value}` pair straight from
  the last non-empty bucket (erasing it there, reclaiming an emptied bucket exactly as `SetVal::
  popArbitrary` and `DictVal::remove` do), and have `popitem` call it. That removes the find-by-equality
  round-trip entirely, is DRY with the Set side, and is faster. Risks no documented contract: NaN keys
  stay write-only, Dict stays unordered (popitem's chosen element is already arbitrary).
  A minimal alternative (`if (!v) throw ...`) would leave a NaN key **permanently undeletable**, so it
  is worse — prefer the bucket-walk.
- Proposed test: `tools/tests/scripts/r7_types.ki` (which already pins the NaN-key semantics) — assert
  `d = {}; d[nan] = 900; var kv = d.popitem(); assert len(d) == 0` and that the returned pair's value
  is `900`. Plus a C++ unit in `tools/tests/unit/test_collections.cpp` asserting no crash + correct
  drain of a mixed `{"a": 700, nan: 900}` dict. Must be verified to SEGFAULT on the unfixed build.


### A06-2: `ValueKind::Array` is entirely vestigial — dead enum + 8 unreachable branches; CLAUDE.md describes it as a real type  [severity: LOW] [confidence: confirmed]
- Location: `src/kirito/object.hpp:23` (the enum member); the dead branches at `runtime.hpp:284,436,437,464,492`, `value.hpp:181`, `stdlib_json.hpp:286,326`, `stdlib_tensor.hpp:1586`.
- What: `ValueKind::Array` is declared, but **nothing in the codebase ever produces it**. There is no
  `ArrayVal` class, no `kind()` override returning `Array`, and no `typeName()` of `"Array"`:
  ```
  $ grep -rn "return ValueKind::Array" src/   -> NONE
  $ grep -rn "ArrayVal" src/                  -> NONE
  $ grep -rn '"Array"' src/                   -> NONE
  ```
  So the audit question "verify Array can't be reached from Kirito in a way that breaks" resolves to:
  **it cannot be reached at all** — it is unreachable by construction, which is safe. But every
  `x.kind() == ValueKind::List || x.kind() == ValueKind::Array` guard (8 sites) is a permanently-dead
  branch that costs a comparison and, more importantly, implies to a reader that a second list-like
  type exists and must be handled — so each new list-handling site invites a "did I remember Array?"
  question with no real answer. It is also a live DRY hazard of exactly the kind this round targets:
  the `|| Array` disjunction is duplicated 8 times and is **already inconsistent** — e.g.
  `ListVal::binary`'s `Add`/ordering arms accept `Array`, but `Mul` (`runtime.hpp:475`) does not.
- What is actually wrong (the reportable part): **`CLAUDE.md` documents Array as a real type** —
  "collections `List`, `Set`, `Dict` (plus an internal `Array` — same value model as `List`, no
  literal/constructor exposed to Kirito)". That is not true of the code as it stands; there is no
  internal Array. CLAUDE.md's own rule is that it "must always describe Kirito as it actually is".
- Repro: the three greps above (run against `src/` at this commit); plus no Kirito-visible probe can
  produce a value whose `type()` is `Array` — `type([])` is `List`.
- Impact: reader/maintainer confusion only; no runtime defect. Ranked LOW deliberately.
- Proposed fix: pick ONE of —
  (a) drop `ValueKind::Array` and the 8 `|| Array` branches, and delete the Array clause from
      CLAUDE.md's type list; or
  (b) if a future `Array` is genuinely planned, leave the enum but say so in CLAUDE.md ("reserved,
      not yet implemented") rather than describing it as existing.
  (a) matches the codebase as built and removes the inconsistency; it risks no contract, since no
  behaviour can observe the difference. Do NOT do this blind — confirm with the maintainer which of
  (a)/(b) is intended, since the enum may be a deliberate placeholder.
- Proposed test: n/a for (a) beyond the suite staying green (the branches are unreachable, so no test
  can cover them — which is itself the argument for removal). If (b), no test.

---

## Coverage audit — per-method table

Legend: **C++** = `tools/tests/unit/*.cpp` · **.ki** = `tools/tests/scripts/*.ki` · **BOTH** · **NONE**.
Every "NONE" was verified by direct grep, not inferred.

### List

| Method / feature | Covered | Where |
|---|---|---|
| append | BOTH | `test_collections.cpp:30`; `r4_collections.ki:180` |
| pop() / pop(i) / pop(-i) / pop(OOB) / pop(empty) / pop(non-Int) | .ki | `r4_collections.ki:181-187`; `labx_containers.ki:65-70` |
| insert (+ negative + clamping) | .ki | `r4_collections.ki:194-196`; `labx_containers.ki:55-60` |
| remove / remove(missing) | .ki | `r4_collections.ki:202-204`; `labx_containers.ki:74-76` |
| index / index(start) / index(end, incl. negative) | BOTH | `test_collections_deep.cpp:27-30`; `r4_collections.ki:205-208` |
| extend (List/Bytes/Set/String; non-iterable throws) | .ki | `r4_collections.ki:242-246`; `labx_containers.ki:87-90` |
| copy (shallow, aliasing pinned) | .ki | `r4_collections.ki:247-251` |
| clear | .ki | `r4_collections.ki:252-254` |
| count (incl. structural equality) | .ki | `r4_collections.ki:210-211` |
| reverse | .ki | `r4_collections.ki:214-216` |
| sort() / sort(key=) / sort(reverse=) / stability | BOTH | `test_sort.cpp:17-32,42-53,80-91`; `r4_collections.ki:217-232` |
| apply | BOTH | `test_apply.cpp:18-29`; `spec_apply.ki:3` |
| `+` concat (+ non-List throws) | BOTH | `test_list_ops.cpp:37-40`; `r4_collections.ki:257` |
| `*` repeat (0 / negative) | .ki | `r4_collections.ki:258` |
| `*` huge-count guard | .ki only | `labx_containers.ki:46` (loose `"too large"` match) |
| lexicographic `< <= > >=` | BOTH | `test_list_ops.cpp:21-31`; `r4_collections.ki:259` |
| negative / OOB indexing | BOTH | `test_collections.cpp:19,34`; `r4_collections.ki:162-163` |
| slicing (start/stop/step/neg/zero/OOB) | BOTH | `test_adversarial.cpp:82-86`, `test_collections_deep.cpp:47-50`; `r4_collections.ki:165-167` |
| iteration / `in` / len | BOTH | `test_collections.cpp:17,21-27`; `labx_containers.ki:40,268-274` |

### Dict

| Method / feature | Covered | Where |
|---|---|---|
| keys / values / items | .ki | `r4_collections.ki:375-377` |
| get(hit / miss / default) | BOTH | `test_collections.cpp:37`; `r4_collections.ki:380`, `:463` (out-of-order kwargs) |
| pop(key) / pop(default) / pop(missing) | .ki | `r4_collections.ki:382-384` |
| popitem / popitem(empty) | .ki | `r4_collections.ki:396-398`; `r8_types.ki:364-366` |
| **popitem on a Dict with a NaN key** | **NONE** | **→ A06-1 (SEGFAULT)** |
| remove / remove(missing) | .ki | `r4_collections.ki:391-394` |
| update(dict) / update(pairs) / bad pair | .ki | `r4_collections.ki:401-406` |
| **update(self) — `d.update(d)`** | **NONE** | **→ A06-3** |
| setdefault(existing / new / no default) | .ki | `r4_collections.ki:386-388` |
| clear / copy (shallow) | .ki | `r4_collections.ki:411-420` |
| apply | BOTH | `test_apply.cpp:37-46`; `r4_collections.ki:423-426` |
| contains / `in` (keys not values) | BOTH | `test_apply.cpp:43`; `r4_collections.ki:363` |
| `d[k]` get/set, missing key, unhashable key | BOTH | `test_collections.cpp:37-38,47-48`, `test_collections_deep.cpp:53-55`; `r4_collections.ki:359-364` |

### Set

| Method / feature | Covered | Where |
|---|---|---|
| add | BOTH | `test_collections.cpp:54-59`; `r4_collections.ki:296-299` |
| discard / discard(missing) | .ki | `r4_collections.ki:300-304` |
| remove / remove(missing) | .ki | `r4_collections.ki:305-308` |
| contains | BOTH | `test_collections.cpp:52-53`; `r4_collections.ki:310-311` |
| union / intersection / difference / symmetricdifference | BOTH | `test_collections_deep.cpp:33-36`, `test_audit_hardening.cpp:72-76`; `r4_collections.ki:327-330` |
| issubset / issuperset / isdisjoint | BOTH | `test_collections_deep.cpp:37-39`; `r4_collections.ki:331-333` |
| pop / pop(empty) | .ki | `r4_collections.ki:312-314` |
| clear / copy | .ki | `r4_collections.ki:315-324` |
| apply (collisions collapse) | BOTH | `test_apply.cpp:32-34`; `r4_collections.ki:348-349` |
| operator `-` / `< <= > >=` (+ non-Set throws) | .ki | `r4_collections.ki:338-344` |
| **self-as-argument, ALIASED (`s.union(s)`, `s - s`)** | **NONE** | **→ A06-3** |
| unhashable member | BOTH | `test_collections_deep.cpp`, `test_adversarial.cpp:75`; `r4_collections.ki:292,309,318` |

### Array
| — | n/a | No test can exist; the type is vestigial (**A06-2**). |

### Cross-cutting

| Scenario | Covered | Where |
|---|---|---|
| sort key that MUTATES (appends) mid-sort | BOTH | `test_audit_hardening.cpp:46-53` (under `setGcThreshold(1)`); `spec_audit_hardening.ki:27-32` |
| **sort key that CLEARS mid-sort** | **NONE** | only `apply` has the clearing variant (`test_audit_v112.cpp:105-111`) → A06-3 |
| sort key that THROWS (+ list-unchanged rollback) | .ki | `r4_collections.ki:238`; `labx_containers.ki:115-116` |
| mixed non-comparable **elements** | BOTH | `test_sort.cpp:97,103`; `r4_collections.ki:237` |
| **key RETURNING mixed/non-comparable types** | **NONE** | → A06-3 |
| multi-key sort via list-returning key | BOTH | `test_list_ops.cpp:47`; `r4_collections.ki:234-236` |
| cyclic / self-containing: str / len / `==` / hash | BOTH | `test_collections.cpp:68`, `test_adversarial.cpp:65-69,142-145`; `r4_collections.ki:444-449` |
| **`sorted()` on a cyclic list** | **NONE** | → A06-3 (probed live: does NOT crash) |
| mutation during iteration (snapshot semantics) | BOTH | `spec_iter_mutation.ki` (dedicated, exhaustive); `test_audit_v112.cpp:105` |
| Dict/Set rehash mid-iteration | indirect | structurally precluded by snapshot iteration; pinned by `spec_iter_mutation.ki` |
| reentrant `_hash_`/`_eq_` mutating ("changed size…") | BOTH | `test_audit_hardening.cpp:101-187`; `spec_reentrant_clear.ki` |
| reentrant `_str_` mutating mid-stringify | .ki only | `spec_audit_v115.ki:26-51` |
| NaN Dict key / Set element (write-only) | BOTH | `test_collections_deep.cpp:67`, `test_audit_fixes.cpp:31`; `r7_types.ki:83-89` |
| str-vs-repr (`['a']`, `['']`, `{'k': 'v'}`) | BOTH | `test_apply.cpp:22`; `r4_collections.ki:436-441` |

### A06-3: test-coverage gaps on the collection surface  [severity: LOW-MED] [confidence: confirmed]
Untested surface is a finding this round. Each verified by grep; each probed live by me (all pass
except the popitem/NaN case, which is A06-1 — i.e. **the one uncovered path is the one that crashes**).

1. **`Dict.popitem` with a NaN key — NONE.** Directly masked A06-1 (a SEGFAULT). `popitem` is the
   *only* observable path to a write-only NaN key (`get`/`in`/`remove` all miss by design per
   `r7_types.ki:87-88`), and it is exactly the path with no test. Six scripts contain both `nan` and
   `popitem` tokens but never in one assertion. **Test:** `r7_types.ki`, beside the existing NaN-key
   block — `d[nan]=900; var kv = d.popitem(); check(kv[1] == 900 and len(d) == 0, "popitem removes a NaN key")`.
2. **sort key that CLEARS the container — NONE.** `test_audit_hardening.cpp:46` covers an *appending*
   key; `test_audit_v112.cpp:105` covers a *clearing* fn for `apply`/Set.apply but not `sort`. The
   clearing case is the stronger one (it drops the snapshot's last other reference). **Test:** mirror
   `test_audit_v112.cpp:105` for `sort` under `okGc1`, with **String** elements (not interned ints):
   `var xs = ["aa","bb","cc"]` + `var f = Function(x): xs.clear(); return x` + `xs.sort(key=f)`.
   Probed live: correct today (`[1000,2000,3000,4000,5000]`), so this pins existing behaviour.
3. **Aliased Set self-argument — NONE.** `r4_collections.ki:343` uses two *distinct* `Set([1])`
   objects, so the genuine aliasing case (`s.union(s)`, `s.difference(s)`, `s - s` on ONE object) is
   untested — and it is the case that could iterate a container it is mutating. **Test:** `r4_collections.ki`
   — `var s = Set([1000,2000]); check(len(s.union(s)) == 2 and len(s - s) == 0 and len(s.difference(s)) == 0
   and len(s.symmetricdifference(s)) == 0 and s <= s and not (s < s), "Set ops with the SAME object aliased")`.
   Probed live: all correct.
4. **`d.update(d)` self-update — NONE** (zero hits repo-wide). **Test:** `r4_collections.ki` —
   `var d = {"a":1000,"b":2000}; d.update(d); check(len(d) == 2 and d["a"] == 1000, "Dict self-update is a no-op")`.
   Probed live: correct.
5. **`sorted()` on a cyclic list — NONE.** `str`/`len`/`==`/hash of cycles are covered; ordering is
   not, and `kiLessThan` recurses into nested lists (guarded by `EqualsGuard`). **Test:** `r4_collections.ki` —
   assert `sorted([a, b])` terminates for mutually-cyclic `a`/`b`. Probed live: terminates, no crash.
6. **A `key=` callback that RETURNS mixed/non-comparable types — NONE.** Mixed *elements* are covered;
   a key that returns e.g. a String for one element and an Integer for another is not. **Test:**
   `r4_collections.ki` — `check(throws(Function(): return [1000,2000].sort(key = Function(x): return x if x > 1500 else String(x))), "sort key returning mixed types throws")`.
7. **`*` huge-count guard has no C++ unit** and the `.ki` match is the loose substring `"too large"`,
   not the exact `"repeated List too large"` (`runtime.hpp:483`). **Test:** add to
   `tools/tests/errors/` or assert the exact message in `labx_containers.ki:46`.
8. **Spelling asymmetry (note, not a gap):** `test_sort.cpp` tests only the **positional**
   `sort(None, True)` form; the `.ki` suites test only the **keyword** `sort(key=…, reverse=…)` form.
   Both are covered, but only jointly across the C++/`.ki` boundary — a single-suite run of either
   alone would miss one spelling.

---

## Non-findings (probed, correct — inputs recorded so the next round need not re-probe)

All probes used values **outside the small-int intern range** (`1000/2000/3000…`, `-1000/-5000`) and
were re-run under **`--gc-threshold 1`** where a callback/allocation was involved. All clean.

- **List slicing, every combination** — `[:]`, `[1:3]`, `[::2]`, `[::-1]`, `[-2:]`, `[:-2]`, `[3:1]`
  (reversed bounds → `[]`), `[10:20]` (OOB → `[]`), `[-99:99]` (clamps to full), `[4:0:-1]`, `[::-2]`,
  `[1:100:3]`. All correct; identical under `--gc-threshold 1`.
- **List error paths** — OOB index → `index out of range`; `[::0]` → `slice step cannot be zero`;
  float index → `index must be Integer, not 'Float'`; `xs * 1000000000` → `repeated List too large`;
  `* -3` / `* 0` → `[]`; `[1000] + 5` → `can only concatenate List to List, not 'Integer'`;
  `[1000] < 5` → `cannot order 'List' and 'Integer'`; pop empty/OOB, remove/index missing — all clean,
  catchable, with line:col.
- **sort with a mutating key** — a key that `clear()`s or `append()`s to the list being sorted:
  snapshot-and-writeback holds; result `[1000,2000,3000,4000,5000]` / `[3000,4000,5000]` (len 3);
  no UAF, identical under `--gc-threshold 1`. A key that **throws** propagates cleanly and leaves the
  list **unchanged** (`[5000,3000,4000]`). Mixed non-comparable elements → `cannot order 'String' and
  'Integer'`, list untouched.
- **sort stability** — a constant-returning key leaves original order (`[3000,1000,2000]`);
  multi-key list-returning key → `[['a',1000],['a',3000],['b',1000],['b',2000]]`. `reverse=` accepts a
  truthy non-Bool (`reverse = 1000`); `key = None` is the identity.
- **Dict full surface** — keys/values/items/get(hit,miss,default)/setdefault(new,existing)/pop(key,
  default)/apply/copy/contains; `update` from a Dict and from pairs (bad pair → `update: each pair must
  have exactly 2 elements (key, value)`); popitem-empty → `popitem: dictionary is empty`; missing key →
  `key not found: zz`; unhashable key → `unhashable type 'List'`; `remove` missing → `key not found: nope`.
- **`d.update(d)` (self)** — no-op, `len` stays 2. Correct (`pairs()` snapshots first). *Untested → A06-3.*
- **Set full surface + operator algebra** — union/intersection/difference/symmetricdifference/issubset/
  issuperset/isdisjoint/contains/copy/apply/pop/clear/discard/remove all correct; `a - b`, `a < b`,
  `a <= b`, `b > a`, `b >= a`, `a < a` (False), `a <= a` (True) all correct.
- **Set self-as-argument, ALIASED** — `s.union(s)` → 3 elems, `s.intersection(s)` → 3,
  `s.difference(s)` → `[]`, `s.symmetricdifference(s)` → `[]`, `s - s` → `[]`, `s <= s` True,
  `s < s` False, `s.isdisjoint(s)` False. All correct. *Untested → A06-3.*
- **`{} <= a` throws `type 'Dict' does not support this binary operator`** — NOT a bug: `{}` is an
  empty **Dict** literal (Kirito has no empty-Set literal); use `Set()`.
- **Cycles / self-containment** — `xs.append(xs)`: `String(xs)` → `[1000, 2000, [...]]`, `len` 3,
  `xs in xs` True. `d["self"] = d` → `{'self': {...}}`. Cyclic `==` → catchable `maximum comparison
  recursion depth exceeded (cyclic structure?)`. `sorted([a, b])` on mutually-cyclic lists terminates.
  Self `extend`/`+`/`insert` all correct (`[1000,2000,1000,2000]`, `[[...], 1000, 2000]`).
- **Mutation during iteration** — List append / Dict insert / Set add mid-loop: iteration is over a
  **snapshot**, loop sees the original elements, container grows correctly. No "changed size" error,
  no crash. Matches `spec_iter_mutation.ki`'s pinned snapshot semantics.
- **Reentrancy guards** — a `_str_` that mutates the Dict mid-stringify → catchable `Dict changed size
  during a key comparison` (no UAF). A `_hash_` that clears the Dict succeeds and yields `len 1` —
  correct by design: `hash()` runs *before* any bucket reference is cached (`collections.hpp:153`).
- **`apply` with an fn that clears the receiver** — List/Dict/Set all return the full mapped result
  (`[1001,2001,3001]` / `{'a': 1001}` / `[1001,2001]`) with the receiver emptied. Snapshot rooting holds.
- **Large collections** — 200k-element List (build/sort/reverse/slice/count) and 100k-entry Dict+Set
  (insert/lookup/contains/full iteration): all correct, no guard misfire, no slowdown cliff.
- **`sorted()` builtin** — stable with `key=`; `reverse=True`; no-args; over a Set / Dict (keys) /
  String (chars). All correct.
- **Argument binding** — out-of-order keywords (`d.get(default = 9000, key = "zz")` → `9000`);
  unknown keyword → `get() got an unexpected keyword argument 'bogus'`; a keyword call skipping a
  required slot → `insert() missing required argument 'index'` (the A05-2 `minArgs` fix holds);
  arity errors → `append expected 1 argument` / `count expected 1 argument`.
- **Key collapse / equality-hash agreement** — `1000` and `1000.0` collapse to ONE key (`1 == 1.0` is
  True); `0.0` and `-0.0` collapse (exact IEEE-754 `0.0 == -0.0`); `True` and `1` stay **distinct**
  keys — consistent, because `True == 1` is **False** in Kirito (Bool is a distinct type). Hashing
  agrees with `==` in every case probed. No finding.
- **Negative values below the intern range** — `[-1000,-3000,-2000].sort()` → `[-3000,-2000,-1000]`;
  `d[-5000] = -9000` get/`.get` both `-9000`; `{-1000,-2000}` sorted + `contains` correct.
- **`Set.pop()` on a NaN member is SAFE** — returns `nan`, `len` → 0. `SetVal::popArbitrary` walks the
  bucket directly instead of re-finding by equality, which is exactly the fix A06-1 wants for `popitem`.
- **`probeBucket` uses raw `Object::equals`, not `kiEquals`** — so a user class's `_eq_` does **not**
  run during bucket probing; instance keys match by identity. A `_hash_`-stable/`_eq_`-always-False
  class therefore does NOT reproduce A06-1 (`popitem` returned `[<Weird object>, 500]` correctly).
  NaN is the only key type that is genuinely unfindable. Recorded so the next round doesn't chase it.

## Summary

| ID | Symptom | Sev | Confidence |
|---|---|---|---|
| **A06-1** | `Dict.popitem()` **SEGFAULTS** on a Dict with a NaN key (unchecked `*find()`) | **HIGH** | confirmed |
| A06-2 | `ValueKind::Array` vestigial — dead enum + 8 unreachable branches; CLAUDE.md describes it as real | LOW | confirmed |
| A06-3 | 7 coverage gaps (incl. the popitem/NaN path that masked A06-1) + 1 spelling-asymmetry note | LOW-MED | confirmed |

The collection surface is otherwise in **very good shape**: every method, argument form, error path,
resource guard, reentrancy guard, snapshot semantic and GC-rooting path I probed behaved correctly,
including under `--gc-threshold 1` with non-interned values. A06-1 is the single functional defect —
and it sits precisely on the one path with no test, which is this round's thesis restated.

Status: DONE
