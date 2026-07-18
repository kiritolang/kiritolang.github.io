# A14 — Kirito-authored frozen-source stdlib modules

Scope: `src/kirito/stdlib_kimodules.hpp` (itertools, functools, collections, statistics,
string, textwrap, base64, csv, tabular, xml, heapq, bisect, copy, enum, tee, arg, semver).

Findings below. Each verified against `./build-debug/ki`.

### F14-1 [Med] statistics.quantiles: tail cut-points wrong (clamps value instead of extrapolating index)
- stdlib_kimodules.hpp:459-480 — `quantiles` (exclusive method). When a cut position `i*(ld+1)/n`
  falls below 1 or at/above `ld`, the code clamps to the *endpoint value* (`s[0]` / `s[ld-1]`).
  CPython's `statistics.quantiles` clamps the *index* j into `[1, ld-1]` but keeps interpolating with
  the (possibly negative or >n) delta, i.e. it EXTRAPOLATES past the data at the tails.
  / Trigger: `quantiles([10,20,30,40,50], 10)` → Kirito `[10.0,12.0,...,50.0]`, Python
  `[6.0,12.0,...,54.0]`; `quantiles([1,2], 4)` → Kirito `[1.0,1.5,2.0]`, Python `[0.75,1.5,2.25]`.
  Middle cut-points agree; only the extrapolation regime (n large relative to ld, or extreme cuts)
  diverges. Silent wrong statistical output, no error.
  / Fix idea: mirror CPython — `j = clamp(Integer(i*(ld+1)/n), 1, ld-1)`, `delta = i*(ld+1) - j*n`,
  `result = (s[j-1]*(n-delta) + s[j]*delta)/n`.
  / Test to add: `spec` asserting `quantiles([1,2],4)==[0.75,1.5,2.25]` and a decile case.
  / Verified-real: CONFIRMED (diffed against CPython statistics.quantiles).

