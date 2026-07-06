# v1.15 audit — Kirito-authored data/functional modules

Subsystem: itertools, functools, collections, heapq, bisect, copy, enum
Source: `src/kirito/stdlib_kimodules.hpp`
Probe binary: `./build-debug/ki`

## LOG
- Read itertools (lines 14-257), functools (260-297), collections (300-369), heapq (762-846),
  bisect (849-881), copy (884-966), enum (969-1003).
- Forming hypotheses; probing next.

## FINDINGS
(appending as confirmed)

### F1 [SUSPECT / BY-DESIGN] Counter.mostcommon(n) with negative n returns an end-slice, not []
- where: src/kirito/stdlib_kimodules.hpp:339-343 (`mostcommon`)
- repro:
```
var c = import("collections")
var cnt = c.Counter(["a","a","b","c","c","c"])
io.print(cnt.mostcommon(-1))   # => [['c',3],['a',2]]  (drops last)
```
- actual: `pairs[0:n]` with n=-1 -> `pairs[0:-1]` drops the LAST element, returning all-but-least-common.
  expected: Python `most_common(-1)` returns `[]` (negative n yields nothing, like heapq.nlargest).
- fix idea: clamp `if n <= 0: return []` before slicing (mirrors heapq.nlargest guard at line 822).
- NOTE: docs/pages/10-stdlib.md:108-110 EXPLICITLY document this end-slice behavior ("don't pass a
  negative `n` expecting an empty list"). So it is deliberate + documented, NOT a defect. Diverges from
  Python (which returns []); a footgun, but by-design. Left here only as a design-consistency flag —
  do NOT "fix" unless the design decision is being revisited.

### F2 [MED] itertools.islice with negative start silently returns wrong window
- where: src/kirito/stdlib_kimodules.hpp:48-59 (`islice`)
- repro:
```
var it = import("itertools")
io.print(it.islice([0,1,2,3,4,5], -2, 4))   # => [0,1,2,3]
```
- actual: negative `start` makes `i >= start` always true and shifts the `(i-start)%step` phase, so it
  behaves like start=0 (or a shifted step phase). No error.
  expected: Python islice rejects negative indices ("Indices for islice() must be None or an integer:
  0 <= x"). Should throw, or at least clamp start>=0.
- fix idea: `if start < 0: throw` (and document that stop must be a non-negative Integer, not None).

## RULED OUT / VERIFIED CORRECT (probed, no defect)
- copy/deepcopy: shallow shares inner refs, deep does not; deep PRESERVES sharing (id(dc[0])==id(dc[1]))
  and handles List/Dict self-cycles without infinite loop (memo-by-id, iterative). Solid.
- copy.copy on a class INSTANCE is a DEEP (independent) copy, not shallow — divergence from Python but
  fully documented (code comment + docs/10-stdlib.md:232-235). By-design (no attribute introspection).
- functools.reduce: empty+no-init throws; empty+init returns init; `initial=None` is a real seed (id
  sentinel, not ==None); 2-arg func contract. All correct.
- functools.partial: single-list-arg contract (func must accept one List) — documented (docs:285). No
  kwarg support, but by-design given Kirito's call model. Snapshot of `bound` verified.
- functools.cache: memoizes (verified call-count), unhashable (List) arg throws cleanly. Correct.
- itertools order matches CPython exactly: permutations, combinations (ascending lexicographic),
  product. r=0 -> [[]]; r>n / r<0 -> []; empty pool -> []. Resource guards fire (>10M throws, no OOM).
- itertools count (float/neg step, exclusive stop), repeat(neg->[]), cycle(0->[]), chain, accumulate,
  takewhile/dropwhile/filterfalse/compress/pairwise(single->[])/ziplongest/groupby(unsorted, keeps
  consecutive runs)/starmap(single-List contract): all correct.
- collections.deque: append/appendleft/pop/popleft correct; empty pop/popleft throw "pop from empty
  List"; iteration + indexing correct. NO rotate/maxlen — but docs (10-stdlib.md:90-98) don't claim
  them, so not doc-drift, just unimplemented Python features.
- collections.Counter: add/get/[]/items/mostcommon(stable desc sort) correct. No arithmetic/subtract/
  negative counts — docs don't claim them. (See F1 for the negative-n footgun.)
- collections.defaultdict: factory-on-missing creates+stores; `in` doesn't create; factory-that-throws
  propagates. Correct.
- heapq: heapify returns a NEW valid heap without mutating input; heappush/heappop maintain heap order
  (pops fully sorted); nsmallest/nlargest guard n<=0 -> []; heapreplace empty throws. NO key= arg on
  nlargest/nsmallest — docs (10-stdlib.md:347-348) don't claim key, so unimplemented, not a defect.
- bisect: bisectleft/right correct on duplicates (left=1,right=4 for value 2 in [1,2,2,2,3]); insort;
  descending-list precondition documented.
- enum: get/nameof/names(definition order)/values; duplicate member throws; nameof(unknown) throws
  ("key not found: N" — raw but does raise); [] and `in` work. Correct.

## SUMMARY
Subsystem is solid. 1 genuine silent-error bug (F2: islice negative start), 1 documented footgun (F1).
Everything else probed clean and matches CPython semantics where a parallel exists.
