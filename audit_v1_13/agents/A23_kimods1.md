# A23 — ki-authored modules subset 1 (itertools, functools, collections, heapq, bisect, copy, enum)

Scope: the frozen Kirito source blocks in `src/kirito/stdlib_kimodules.hpp`
(itertools L14–228, functools L231–268, collections L271–340, heapq L727–811,
bisect L814–846, copy L849–931, enum L934–964). Registered via
`vm.registerSourceModule`, compiled once per VM on first import. READ-ONLY audit;
static reasoning only (no build/run).

Tests examined: `tools/tests/unit/test_kimods_deep.cpp`, `test_stdmodules.cpp`,
and scripts `deep_kimods.ki`, `r4_kimods_a.ki`, `r7_kimods_a.ki`, `r10_kimods.ki`,
`audit_bisect.ki`, `audit_heapq.ki`, plus `r6/r7/r8_kimods_*`.

## Headline

Coverage for these seven modules is unusually strong. The dedicated
`deep_kimods.ki` / `r4_kimods_a.ki` suites already **document, with tests, every
"known limitation"** I would otherwise flag as a bug: duplicate-enum silent
corruption, deque having no `maxlen`/`rotate`, Counter having no arithmetic,
`cache` throwing on unhashable args, `islice` step=0 throwing, `starmap`
passing the whole tuple as one arg (no spread), `count`/`repeat`/`cycle`
requiring bounds. These are accepted design choices given "no generators / no
varargs," not defects. No NEW confirmed logic bug was found in the
implementations. Remaining findings are genuine coverage gaps, resource
weak-spots, and one real (untested) shared-reference limitation in `deepcopy`.

Counts: Confirmed bugs 0 · Weak-spots 4 · Coverage-gaps 8 · DRY/efficiency 3.

---

### A23-1: deepcopy does NOT preserve shared references that cross the serde/instance boundary
- severity: Medium
- location: `stdlib_kimodules.hpp` copy `deepcopy` L877–930 + `_copyViaSerde` L858–863
- category: correctness / shared-reference preservation
- description: `deepcopy`'s memo machinery (map each original container `id`→one
  shell) correctly dedups shared refs **within the List/Dict/Set graph** (tested
  L305–308). But a user class instance / native value object is copied by a
  SEPARATE call to `_copyViaSerde(cur)` (L905), which independently deep-copies
  everything reachable *through the instance*. Containers reachable only through
  the instance are never registered in `memo`. So if the SAME list `L` appears
  both directly in the outer structure AND inside an instance's attribute, the
  outer occurrence gets the memo shell while the instance's occurrence gets a
  distinct serde copy → two copies where Python's memo-based deepcopy yields one.
  Identity link is silently broken.
- failure-scenario: `var L=[1]; class C: var _init_=Function(self,x): self.ref=x`
  then `var g=c.deepcopy([L, C(L)])` → `id(g[0]) != id(g[1].ref)` even though
  `id(L)==id(L)` in the original. (Same-instance-appearing-twice IS deduped via
  memo L893; only the cross-boundary case fails.)
- proposed-test: build a list that shares a sublist with an instance attribute,
  deepcopy it, assert the two copies share identity — currently they won't.
- proposed-fix: hard to fix in pure Kirito (no attribute enumeration); at minimum
  document the limitation next to L853's comment. A real fix needs the serde
  codec to accept/return a shared memo, or a native attribute-enumeration hook.
- confidence: High (that the gap exists); Medium (that it matters in practice —
  mixing container graphs with instance graphs is uncommon).

### A23-2: `_copyViaSerde` swallows ALL exceptions and returns the ORIGINAL object (silent aliasing)
- severity: Medium
- location: `_copyViaSerde` L858–863 (used by both `copy` and `deepcopy`)
- category: error-handling / silent aliasing
- description: `try: return serialize.loads(serialize.dumps(obj)) catch as e:
  return obj`. On ANY serialize failure (a live socket/file, a compiled Regex, a
  gradient-tracking Tensor, a Function, or any type serialize rejects) it returns
  the **same object**, so `copy`/`deepcopy` of such a value is an alias, not a
  copy — mutating the "copy" mutates the original. Documented as "best effort"
  (L857) but the blanket `catch as e` also masks *genuine* serde bugs (a real
  crash inside dumps/loads is silently downgraded to aliasing).
- failure-scenario: `var d=c.deepcopy(someFunctionOrSocket)`; `id(d)==id(orig)`.
  Also a latent serde regression would be hidden instead of surfacing.
- proposed-test: `copy`/`deepcopy` of a Function and of an open file — assert the
  returned identity (documents the aliasing), and separately of a plain instance
  (must differ).
- proposed-fix: narrow the catch to serde's "not serializable" error type/message
  and re-throw anything else; or at least note the aliasing risk in docs.
- confidence: High (behavior); Low (severity — matches documented intent).

### A23-3: `islice` cannot express an unbounded stop (`None`), unlike Python
- severity: coverage-gap / Low feature-gap
- location: itertools `islice` L48–57
- category: feature parity / error-quality
- description: Python's `islice(it, start, None[, step])` slices to the end.
  Here `stop` is required and `None` is not special-cased, so `islice(xs, 2,
  None)` hits `if i >= stop` = `Integer >= None` → an ugly comparison-type throw,
  not a clean message and not the "to the end" behavior. `islice(xs, stop)`
  one-arg form also unsupported (start required). Both are accepted design
  ("start AND stop required", tested L44), but the `None` case gives a confusing
  error rather than a deliberate one.
- failure-scenario: `it.islice([1,2,3], 1, None)` throws a comparison error.
- proposed-test: assert `islice(xs, 1, None)` either slices-to-end or throws a
  *clear* "stop must be an Integer" message.
- proposed-fix: treat `stop == None` as "to end", or throw an explicit
  islice-specific message.
- confidence: High.

### A23-4: `islice`/`count` negative `start`/bounds accepted silently (differ from Python's ValueError)
- severity: coverage-gap / Low
- location: itertools `islice` L54 (`i >= start`), `count` L15–26
- category: input validation
- description: a negative `start` in `islice` makes `i >= start` always true and
  `(i-start)%step` shift by a positive offset — it silently behaves like
  `start=0`-ish with a phase shift, never an error (Python rejects negatives).
  `count`/`islice` also don't validate `step < 0` for `islice` (`(i-start)%step`
  with negative step yields Kirito's sign-of-divisor modulo → surprising skips).
  Untested.
- failure-scenario: `it.islice([0,1,2,3], -2, 3)` returns something non-obvious
  instead of erroring.
- proposed-test: negative start / negative step inputs — pin whatever the chosen
  contract is.
- proposed-fix: validate `start >= 0`, `step >= 1` and throw a clear message.
- confidence: Medium.

### A23-5: No resource guard on combinatorial explosion (product / permutations / combinations)
- severity: Weak-spot / Medium
- location: itertools `product` L76–87, `permutations` L89–114, `combinations` L116–138
- category: resource exhaustion
- description: CLAUDE.md states native `range`/list-repetition/padding are bounded
  to throw instead of OOMing. These pure-Kirito combinators have **no such
  guard**: `product([[0..9]]*8)` (10^8 rows), `permutations(range(12))` (~479M),
  or `combinations(range(40), 20)` build the entire result List eagerly and will
  OOM / hang the VM rather than throw a clean "result too large". `permutations`
  additionally does an O(k) `i not in chosen` linear scan per node, compounding
  cost. Untested (no huge-input case).
- failure-scenario: `it.permutations(range(0,13))` exhausts memory with no
  catchable error.
- proposed-test: assert a bounded/`throws` outcome (once a guard exists) for a
  deliberately huge product/permutations request.
- proposed-fix: cap the running/output size (mirror the range/repetition guard)
  and throw a catchable "too many combinations" error past a threshold.
- confidence: High (no guard exists); the severity is "policy" — Python also
  doesn't guard, but Kirito's stated policy is that it should.

### A23-6: `deque` — no `maxlen` (no eviction), no `rotate`/`extend`/`clear`/`index`; `_iter_` leaks the backing list
- severity: coverage-gap (already documented) + Low
- location: collections `deque` L272–293
- category: feature parity
- description: constructor is `deque(items=None)` with NO `maxlen` → no bounded
  ring-buffer eviction (the classic deque use). Also missing `rotate`, `extend`,
  `extendleft`, `clear`, `remove`, `index`, `count`. All ALREADY captured by
  tests as accepted findings (deep_kimods L151, L161, L164). Additional
  micro-note: `_iter_` (L292) returns `self._items` — the LIVE backing list — so
  `for x in d` iterates the internal list by reference; mutation during iteration
  or callers stashing the returned list can observe/alter internals. Untested.
- proposed-test: mutate a deque while holding a reference obtained via iteration
  to pin the aliasing behavior; also `col.deque([1,2],5)` already asserts no
  maxlen (L164).
- proposed-fix: (optional) add `maxlen` with eviction; return a copy from `_iter_`
  if isolation is desired.
- confidence: High.

### A23-7: `Counter` — no arithmetic (+/-/&/|), no `update`/`subtract`/`elements`/`total`/`_setitem_`; `mostcommon` negative-n slice untested
- severity: coverage-gap
- location: collections `Counter` L295–318
- category: feature parity + edge coverage
- description: Counter exposes only `add`/`get`/`items`/`mostcommon`/`_getitem_`.
  Missing the whole arithmetic surface and bulk `update`/`subtract`. The
  no-arithmetic gap is already tested/accepted (deep_kimods L186). Untested edge:
  `mostcommon(n)` with **negative** n → `pairs[0:n]` uses List slicing
  semantics (`pairs[0:-1]` drops the last), which is almost certainly not the
  caller's intent; and `mostcommon(0)` returns `[]`. No test pins negative/zero n.
  `_getitem_` returns 0 for missing (Python parity, good) but there is no
  `_setitem_`, so `c[k] = n` is impossible.
- proposed-test: `c.mostcommon(-1)`, `c.mostcommon(0)`; assert chosen contract.
- proposed-fix: validate/clamp `n`; consider `update`/`_setitem_`.
- confidence: High.

### A23-8: `defaultdict` — no `_len_`, no `_iter_`, no `get`; factory-arity errors unguarded
- severity: coverage-gap / Low
- location: collections `defaultdict` L320–339
- category: feature parity
- description: supports `_getitem_` (auto-inserting), `_setitem_`, `_contains_`,
  `keys`/`values`/`items`, `_str_`. But `len(dd)` throws (no `_len_`), `for k in
  dd` throws (no `_iter_`), and there is no `get(k, default)`. `_getitem_` calls
  `self._factory()` with zero args — a factory needing arguments throws a generic
  arity error rather than a defaultdict-specific message. None tested for the
  missing dunders.
- proposed-test: `len(dd)` / `for k in dd` — pin whether they throw or work.
- proposed-fix: add `_len_`/`_iter_` delegating to `_data`.
- confidence: High.

### A23-9: `enum` — duplicate names silently corrupt; `nameof` on unknown value throws a raw Dict-key error (inconsistent with `get`)
- severity: coverage-gap (duplicate case already tested) + Low
- location: enum `Enum` L935–963
- category: input validation / error-quality
- description: duplicate member names silently overwrite `_byName` while
  `_order`/`values()` keep both — ALREADY tested as an accepted FINDING
  (deep_kimods L352–359). Additional untested inconsistency: `get(name)` throws a
  clean `"no such enum member: <name>"` (L948–949), but `nameof(value)` does
  `return self._byValue[value]` (L952) with NO membership check → an unknown value
  throws a raw missing-key error with a poor message. Also `Enum` values are
  hard-wired to `0..n-1` (no custom values), a feature gap vs Python.
- failure-scenario: `en.Enum(["A"]).nameof(5)` → ugly Dict KeyError message.
- proposed-test: `nameof` on an out-of-range value asserts a *clear* message.
- proposed-fix: guard `nameof` symmetrically with `get`; optionally reject
  duplicate names with a clear error.
- confidence: High.

### A23-10: `cache` never memoizes recursion and throws (not a clean error) on unhashable args
- severity: coverage-gap / Low (both documented behaviors)
- location: functools `cache` L261–267
- category: semantics
- description: `cache(f)` returns a wrapper; the memo only intercepts calls
  through the wrapper, so a self-recursive `f` that calls itself by its original
  name is NOT memoized (Python's `@cache` rebinds the name, so it is). Unhashable
  arg (`[1,2]`) makes `x not in store` throw a dict-key error — tested & accepted
  (deep_kimods L144). Multi-arg functions can't be cached (single `x` param, by
  design — the module's list-arg convention isn't applied here). No test for the
  recursion-not-memoized behavior.
- proposed-test: cache a naive recursive fib, assert the inner recursion still
  recomputes (documents the boundary).
- proposed-fix: none needed (design); document that `cache` memoizes only the
  outer call.
- confidence: High.

### A23-11: `heapq.merge` and `heapify` are O(N log N) — they ignore the efficiency contract of their names
- severity: dry / efficiency / Low
- location: heapq `heapify` L773–777, `merge` L801–810
- category: efficiency / DRY
- description: `heapify` builds the heap by N successive `heappush` (O(N log N))
  rather than the standard bottom-up O(N) sift. `merge` pushes EVERY element of
  every input list into one heap and drains it (O(N log N)) — it never exploits
  that the inputs are already sorted (a real k-way merge is O(N log k)). Results
  are correct; only the complexity contract is off. `nsmallest` also fully
  heapifies then pops (fine for small n, wasteful for tiny n vs large input).
- proposed-test: n/a (behavioral tests pass); this is an efficiency note.
- proposed-fix: bottom-up `heapify`; a true tournament `merge`. Low priority.
- confidence: High.

### A23-12: List-of-args calling convention (chain/product/starmap/partial) is consistent but easy to misuse; `starmap`/`partial` do NOT spread
- severity: dry / Low
- location: itertools `chain` L41, `product` L76, `starmap` L174–178; functools `partial` L250–259
- category: DRY / ergonomics
- description: because Kirito has no varargs, `chain(lists)`, `product(lists)`,
  `ziplongest(lists)`, `merge(lists)`, `starmap(func, argtuples)` and
  `partial(func, bound)` all take a SINGLE list rather than Python's `*args`.
  `starmap` calls `func(args)` (one list arg, no spread — tested L100–101) and
  `partial` calls `func(allargs)` (one list arg). This is coherent across the
  modules and documented in comments, but it diverges from every user's Python
  mental model and there's no cross-module helper capturing the "freeze a list of
  args then call f(list)" pattern shared by `partial` and `starmap`. Note only.
- proposed-fix: none (design); ensure docs state the list-arg convention loudly.
- confidence: High.

### A23-13: `accumulate` leading-comment overstates behavior for a leading `None` with the default `+`
- severity: Low / doc-accuracy
- location: itertools `accumulate` L59–74
- category: comment accuracy
- description: the comment (L60–61) says `accumulate([None, 1, 2])` "folds the
  leading None instead of treating it as not started yet." It does set
  `total = None` for the first element, but the SECOND element then evaluates
  `total + x` = `None + 1`, which THROWS (no `_add_` for None). So with the
  default reducer a leading `None` doesn't "fold" — it errors on the next step.
  The identity-vs-`==None` distinction the comment defends is real and correct;
  the illustrative claim is just misleading. Untested for a leading-None input.
- proposed-test: `accumulate([None, 1])` — assert it throws (documents reality).
- proposed-fix: reword the comment to "a leading None becomes the seed; the next
  `+` will then fail unless a custom func is supplied."
- confidence: High.

---

## Coverage matrix (per function: tested? / notable untested angle)

itertools: count ✓(step0, bounds) · repeat ✓(requires times) · cycle ✓ ·
chain ✓ · islice ✓(step0, start>stop, cap) — GAP: None-stop, negative start ·
accumulate ✓(func + default) — GAP: leading None · product ✓(empty factor,
zero lists, copy-independence) — GAP: huge explosion · permutations ✓ — GAP:
negative r (returns [] vs Python error), huge · combinations ✓ — GAP: huge ·
takewhile/dropwhile/filterfalse/compress/pairwise/groupby/ziplongest ✓ ·
starmap ✓(no-spread, too-short throws).

functools: reduce ✓(empty+initial sentinel) · partial ✓(snapshot) ·
cache ✓(memo, unhashable throws) — GAP: recursion-not-memoized.

collections: deque ✓(ends, no maxlen/rotate) — GAP: `_iter_` aliasing ·
Counter ✓(tally, mostcommon, no arithmetic) — GAP: negative/zero mostcommon n ·
defaultdict ✓(auto-insert) — GAP: no `_len_`/`_iter_`/`get`.

heapq: heappush/pop/heapify/heapreplace/merge/nlargest/nsmallest ✓(dups, n<=0,
n>len, floats). Efficiency notes only.

bisect: bisectleft/right/insortleft/right + aliases ✓(dup runs, clamp,
multiplicity, alias identity). Thorough.

copy: copy(shallow, immutables, instance=deep) ✓ · deepcopy(cycle, shared-ref
within container graph) ✓ — GAP: cross-serde-boundary shared ref (A23-1),
unserializable→alias (A23-2).

enum: get/nameof/names/values/_getitem_/_contains_ ✓ + duplicate-corruption
tested — GAP: nameof unknown-value message.

## Top 5
1. A23-1 deepcopy drops shared identity across the serde/instance boundary (real, untested limitation).
2. A23-5 no resource guard on product/permutations/combinations — OOM/hang instead of the stated clean-throw policy.
3. A23-2 `_copyViaSerde` blanket-catches and returns the ORIGINAL (silent aliasing; also masks serde regressions).
4. A23-3 `islice` None-stop gives an ugly comparison error instead of Python's slice-to-end or a clear message.
5. A23-9/A23-8/A23-7 uneven error-quality & missing dunders across enum/defaultdict/Counter (nameof raw KeyError, no `_len_`/`_iter_`, negative mostcommon n).

Note: nearly all "would-be bugs" (duplicate enum, deque maxlen, Counter
arithmetic, cache unhashable, starmap non-spread, islice step0) are ALREADY
tested as accepted design findings in `deep_kimods.ki`/`r4_kimods_a.ki` — this
module family is well-guarded. No confirmed NEW logic bug in the implementations.
