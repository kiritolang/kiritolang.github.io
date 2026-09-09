# A8 — .ki STANDARD LIBRARY (kimodules) — deep audit, scan3 (v1.17.1)

Scope: every Kirito-implemented stdlib module in `src/kirito/stdlib_kimodules.hpp`
(itertools, functools, collections, statistics, string, textwrap, base64, csv, heapq,
bisect, copy, enum, tee, arg, tabular, xml, semver) plus `kpm/kpm.ki`.

Method: all probes run on `build-bin/ki-asan` (source unchanged, no rebuild). Numerics
verified by independent hand computation; complexity claims verified by timing two input
sizes (ratio for 2× input). Hard rule honoured: only user-code-provable findings counted;
reproducers below are real `.ki` programs with actual-vs-expected.

## Headline

**No HIGH or MED confirmed bugs.** This layer is genuinely clean — algorithms correct and
idiomatic, complexity as advertised, error paths fail loud, deep-copy of cycles/shared-refs
correct, semver/csv/xml parsers reject junk rather than fabricate data. Two LOW findings
(both cosmetic / semantics-debatable) and a handful of documented design divergences noted.

---

## Findings

### F1 · LOW · CONFIRMED — textwrap.dedent does not normalize whitespace-only lines (contradicts its own "matching CPython" claim)
`textwrap` · stdlib_kimodules.hpp ~L617-645 (`dedent`)

The code comment states dedent behaves "matching CPython textwrap.dedent". CPython blanks
whitespace-only lines *before* computing the common margin, so such a line becomes empty in
the result. Kirito's dedent instead leaves a whitespace-only line untouched when it doesn't
start with the common prefix.

Reproducer:
```
var tw = import("textwrap")
io.print(tw.dedent("    a\n  \n    b").replace("\n","|"))
```
Actual:   `a|  |b`  (the middle whitespace-only line keeps its 2 spaces)
Expected (CPython parity, independently: `textwrap.dedent("    a\n  \n    b")` → `"a\n\nb"`):
`a||b`

Root cause: the final application loop only strips `prefix` from lines that begin with it;
a whitespace-only line shorter than the prefix passes through verbatim, and dedent never
normalizes whitespace-only lines to empty as CPython does. Cosmetic (stray trailing
whitespace) but a real doc-vs-reality divergence; would break an exact-match golden.

### F2 · LOW · SUSPECTED (semantics debatable) — csv.parse of a lone quoted-empty field drops the row
`csv` · stdlib_kimodules.hpp ~L794-799 (`_parserows` final flush)

Reproducer:
```
var csv = import("csv")
io.print(csv.parse("\"\""))     # actual: []      arguably-expected: [[""]]
io.print(csv.parserow("\"\""))  # [''] (parserow is fine)
io.print(csv.parse("\n"))       # [['']] (a bare blank line DOES yield a row)
```
`parse("\"\"")` (a single explicitly-quoted empty field, no newline, no other fields)
returns `[]`; the same content unquoted as a blank line returns `[['']]`. The flush guard
`current != "" or len(fields) > 0` can't tell "we consumed a quoted-empty field" from
"nothing was there", because the state machine tracks no "saw-a-field" bit.

Impact: effectively nil — this only triggers when the *entire* input is one quoted-empty
field with nothing else (`parse("a,\"\"")` and `parse("\"\",x")` are both correct), and
`readcsv` drops single-empty-field rows anyway. Reported for completeness; the "correct"
result here is genuinely ambiguous (pandas would treat `""` as a header line), so not
counted as a hard bug.

---

## Informational / design divergences (NOT bugs — documented behaviour)

- **tabular Series–Series arithmetic aligns by POSITION, not label.** `Series([1,2,3],[10,20,30]) + Series([100,200,300],[30,20,10])` → `[101,202,303]` (pandas by-label would give `[301,202,103]`). This is explicitly documented in the `_binop` contract ("aligned by position"). Worth a doc callout for pandas migrants but not a defect.
- **copy.deepcopy of a user-instance/native-value that shares a mutable child with an outer container** breaks that specific cross-boundary aliasing (the instance is copied whole via the serialize codec, producing a *separate* copy of the shared child). Documented in the source comment; List/Dict/Set aliasing and cycles are all preserved correctly (see Verified CORRECT).
- **itertools.accumulate([None, 1, 2])** throws on `None + 1` by design (the leading `None` is folded, per the documented comment) — intended, not silent.
- **kpm/kpm.ki resolver**: the constraint intersection / two-phase fixpoint (`resolvePlan`, `pickRef`) is built entirely on the `semver` module, which I verified correct (below). Exercising the resolver end-to-end requires live GitHub/GitLab network calls, so it is not user-code-reproducible offline; no counted finding. The semver primitives it depends on (`satisfies`, `maxsatisfying`, `validrange`, `compare`, numeric ordering) are all correct.

---

## Verified CORRECT (probes + independent checks + timings)

**itertools** — `count(0,1,5)`→[0..4], `count(5,-1,0)`→[5..1], `count(0,2,10)`→[0,2,4,6,8]
(ternary `x < stop if step>0 else x > stop` precedence verified correct);
`permutations([1,2,3],2)` = 6 rows in classic order; `combinations([1,2,3,4],2)` = 6 rows
ascending-lex; `product([[1,2],[3,4]])`=[[1,3],[1,4],[2,3],[2,4]]; `accumulate([1,2,3,4])`=
[1,3,6,10]; `groupby` groups CONSECUTIVE only (verified re-emits key `1` twice);
`pairwise([1,2,3,4])`=[[1,2],[2,3],[3,4]]. Combinatorial guards (`_MAXCOMBINATIONS`) present.

**functools** — `reduce(+,[1,2,3,4],0)`=10; `reduce(*,...)` no-init=24; empty+no-init throws;
`partial` snapshots bound args and prepends correctly (`partial(a-b,[10])([3])`=7); `cache`
memoizes. Sentinel-by-identity for the unset initial is correct.

**collections.deque** — two-stack amortized O(1) CONFIRMED by timing: FIFO fill+drain
n=20k → 40k ratio **1.85** (linear, not quadratic). `_getitem_` positive/negative indices,
`_str_`, iteration after rebalance all correct (`deque([0,1,2,3])`, `dq[-1]`=3).
**Counter** — `mostcommon` sorts by count desc with stable insertion order among ties
(`["b","b","a","a","c"]`→`[[b,2],[a,2],[c,1]]`). **defaultdict** factory + membership OK.

**statistics** — independently hand-checked: `mean([1,2,3,4])`=2.5, `median([1,2,3,4])`=2.5,
`variance([1,2,3,4,5])`=2.5, `pvariance`=2.0, `stdev`=1.5811388300842, `mode([1,1,2,3,3])`=1
(first-seen tie, = multimode[0]), `multimode`=[1,3]. `quantiles([1,2],4)`=[0.75,1.5,2.25]
and `quantiles(1..10,4)`=[2.75,5.5,8.25] both match CPython exclusive method by hand.
Large-integer overflow guard works: `mean([2^62, 2^62])`=4.61e18 (no int64 wrap).

**heapq** — `nsmallest(3,...)`=[1,1,2], `nlargest(3,...)`=[5,4,3], `merge` of 3 sorted lists
= fully merged 1..9. Complexity CONFIRMED linear: `nsmallest` 20k→40k ratio **1.96**,
`nlargest` ratio **2.12** (heap-based, not full-sort). `merge` is a genuine k-way heap merge
with deterministic tie-break.

**bisect** — `bisectleft([1,2,2,2,3],2)`=1, `bisectright`=4; `insort` inserts in order.

**string** — `similarity("kitten","sitting")`=0.5714 (edit dist 3 / longer 7, hand-checked);
`closest("helo",[...])`="hello"; empty-string similarity=1.0 path present.

**base64** — `encode("Man")`="TWFu", `encode("M")`="TQ==", `decode("TWFu")`=[77,97,110],
full roundtrip OK. Rejects (throws) on: bad char `"****"`, lone trailing char `"TWFuA"`,
data-after-padding `"TQ==x"`, byte 256, byte −1 (no silent %256 coercion). urlsafe variant OK.

**csv** — quoted fields with embedded `\n`, `""` un-escaping, CRLF→LF, trailing-newline (no
phantom row), empty interior fields, and full format→parse roundtrip of embedded commas &
newlines all correct. Float cells use `repr()` (round-trippable). Shared `_parserows` core
is single-source for both `parse` and `parserow`. (Only the F2 lone-quoted-empty corner.)

**xml** — parse tag/attrib/text/nested/`find`/`findall`; entity decode `&lt;&gt;&amp;&quot;&apos;&#65;&#x42;`→`<>&"'AB`; malformed numeric entity `&#zz;` kept verbatim (lenient, never crashes); unquoted attribute `b=c` consumed correctly (no spurious empty attrs); `tostring` round-trips with correct escaping. Surrogate/out-of-range numeric refs rejected.

**semver** — 19 satisfies() cases all correct incl. `^`/`~` on 0.x, x-ranges, hyphen ranges,
`||`, space-after-operator `">= 1.2.0"`, prerelease gating (`1.0.0-beta` fails `>=1.0.0` but
passes `>=1.0.0-alpha`), 4-component rejection (`valid`/`validrange` both False on `1.2.3.4`).
Numeric ordering correct: `sort(["1.10.0","1.9.0","1.2.0"])`=[1.2.0,1.9.0,1.10.0].
`maxsatisfying(...,"^1.0.0")`=1.3.0.

**copy** — deepcopy of a self-referential list preserves the cycle (`id(b[2])==id(b)`,
`id(b)!=id(a)`); shared refs preserved (`id(c[0])==id(c[1])`, independent of original,
mutation isolated); dict cycle preserved (`id(e["self"])==id(e)`). Iterative, no overflow.

**enum** — `get`/`nameof`/`values`/definition-order `names`; duplicate-name rejected.

**arg** — positionals, `--opt val`, `--opt=val`, flags, `-5` stays positional, short options,
`--` separator (rest passthrough), unknown-option throw, bad-numeric throw, `rest` bucket.

**tabular** — DataFrame `sum`/`mean` per-column; `groupby.sum`/`mean` (hand-checked
x:{a4,b40}/y:{a6,b60}, means x:{2.0,20.0}/y:{3.0,30.0}); Series `sum`/`mean`/`count`/`median`
skip missing correctly (`[1,2,None,4]`→sum7,mean2.333,count3,median2); non-numeric value in a
numeric aggregation is a HARD ERROR (no silent drop); comparison with missing yields `None`
mask entry (`[5,None,15] > 10`→[False,None,True]); `describe` std hand-checked (1.29099444874);
merge `_x`/`_y` suffixing on colliding non-key columns; duplicate-column rejection present;
NaN group/join keys dropped (write-only-Dict guard). Numeric-column detection gates reductions.

**tee** — read-only inspection; stream fan-out + context-manager save/restore of io.stdout/stderr
look correct (not separately timed; no user-provable defect found).

## Coverage gaps worth a test (not bugs)
- No `.ki` golden for `textwrap.dedent` whitespace-only-line normalization (F1 would be caught).
- No `.experr`/golden pinning `csv.parse("\"\"")` semantics (F2).
- No regression pinning statistics large-integer `mean` (the 2^62 overflow guard) at the .ki level.
- No `.ki` test asserting deque amortized behaviour (timing) — currently only inferred.
