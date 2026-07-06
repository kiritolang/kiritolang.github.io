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
