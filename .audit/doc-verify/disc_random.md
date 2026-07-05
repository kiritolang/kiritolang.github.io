# `random` module — doc-vs-impl verification

Test: `tools/tests/scripts/verify_random.ki` (+ `.expected` = `OK random\n`), 82 asserts.
Impl: `src/kirito/stdlib_random.hpp`. Docs: `docs/pages/10-stdlib.md` `## random`.
Runner: `build-debug/ki`. **No `src/` edits.**

## Discrepancies

**None.** Every documented surface behaves exactly as described.

## Coverage

- **Constructor**: `Random(seed)`, `Random()` (OS-seeded), `seed=` and `generator=` kwargs;
  `.generator` attribute; engine aliases `"xoshiro"`/`"xoshiro256"` and
  `"mersenne_twister"`/`"mt19937_64"`; unknown generator throws `unknown generator ...`; float seed
  rejected; `.generator` is read-only.
- **Determinism**: same-seed generators produce identical streams (xoshiro + mersenne), reproducible
  across 20 draws; different engines with the same seed differ; `seed(a)` reseeds to reproduce and
  does not switch engine kind.
- **Distributions/ranges**: `random()` in `[0,1)`, `uniform(a,b)` in `[a,b)`, `randint(a,b)`
  inclusive, `randrange(stop)`/`randrange(start,stop[,step])` (incl. negative step) yields a
  `range(...)` member; return types (Float/Integer) checked.
- **choice/choices/sample/shuffle**: choice returns a member (+ single-element); choices with
  replacement, right length, default k=1, k=0→[], k>len allowed, single-element repeats; sample
  distinct/subset/permutation at k=len, k=0→[], empty+k=0→[], single-element; shuffle is an in-place
  permutation (sorted-equal), empty no-op.
- **gauss/normalvariate/expovariate**: return Float; `gauss(5,0)==5.0`; **all-default `gauss()` and
  keyword-only `gauss(sigma=2.0)`/`gauss(mu=3.0)` WORK (do not throw)**; expovariate non-negative.
- **Hardening (exact messages PINNED)**:
  - `randint(5,1)` → `randint: empty range`
  - `randrange(0,10,0)` → `randrange: step must not be zero`
  - `randrange(5,5)` / `randrange(0)` → `randrange: empty range`
  - `sample([1,2,3],5)` / `sample(...,-1)` / `sample([],1)` → `sample: k out of range`
  - `choice([])` → `choice from empty sequence`
  - `choices([],2)` → `choices from empty population`
  - `choices([1,2],-1)` → `choices: k must be non-negative`
  - `shuffle("abc")` / `shuffle({1,2})` → `shuffle requires a List`
  - `expovariate(0)` / `expovariate(-1)` → `expovariate: lambda must be positive`
  - `gauss(0,-1)` / `normalvariate(0,-2)` → `gauss: sigma must be non-negative` (needle `sigma`)
- **serialize/dump**: both round-trip a Random and restore the **exact stream** on the same engine,
  tagged with the generator kind (xoshiro + mersenne verified; `dump.dumps` returns `Bytes`).
- **inspect**: lists methods + the `generator` attribute.

## Pinned current behaviour / notes (not bugs)

- `uniform(20, 10)` with **reversed bounds does NOT throw** — it silently returns a value in the
  reversed range (e.g. `~18.0`). This matches Python's `random.uniform` (a>b is permitted), so it is
  intended, not a silent bug. Not documented as an error; left as-is.
- `gauss(sigma=2.0)` works because the leading skipped positional `mu` is hole-filled with `None`
  (makeMethod) and `optNum` treats a `None` slot as "use the default" — a deliberate design (see the
  `optNum` comment in the impl).
- Float seed / float `k` / float `randint` bounds are rejected (`asInt` on the arguments).
- `.generator` reports the canonical engine name even when constructed via an alias
  (`xoshiro256`→`xoshiro`, `mt19937_64`→`mersenne_twister`).
