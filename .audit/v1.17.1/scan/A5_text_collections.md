# A5 — Text & Collections correctness/UB audit (v1.17.1)

Scope: `bytes.hpp`, `regex_engine.hpp`, `stdlib_regex.hpp`, `collections.hpp` (List/Set/Dict internals,
hashing, iteration order), and the String methods + lazy iterators (range/map/filter/zip/enumerate/iter)
in `runtime.hpp`. List/String *slice semantics* excluded (audited separately) — Bytes-slice-returns-Bytes
was checked as it is not part of that separate slice audit.

Method: no compilation. Reproduced on real binaries `build-bin/ki-release` and `build-bin/ki-asan`
(ASAN_OPTIONS=detect_leaks=0, `ulimit -s 262144`). Probes written to /tmp, results hand-verified.

## Verdict: NO confirmed correctness or UB defects found in scope.

Every adversarial probe either produced the correct result or failed cleanly with a catchable,
structured error. ASAN stayed clean on all reentrancy / malformed-input / deep-nesting probes. This
subsystem is exceptionally well-hardened; the "findings" below are all documented design choices or
low-severity performance notes, not bugs.

---

## Reproduced-as-CORRECT (CONFIRMED on a real binary)

### Regex engine (regex_engine.hpp / stdlib_regex.hpp)
- **Linear-time / no ReDoS.** `(a+)+$`, `(a*)*b`, `(a+)+b`, `(.*)*b`, `(a|aa)*b` over 10k `a`s, and
  `(a|a)*b` over 5000 `a`s all return instantly (no hang, `timeout 5`/`8`/`12` never fired). Pike-VM
  guarantee holds.
- **Match-time work budget trips cleanly.** 600 capture groups `(a?)` over 20 000 `a`s throws
  `regex match failed: regex match exceeded its complexity budget ...` (catchable KiritoError, no hang) —
  the `kMaxMatchWork` guard in `addThread` works and is translated at the native boundary by `runGuarded`.
- **Malformed patterns all rejected cleanly** (no crash/UB): `(`, `[a-`, `(?P<n>`, `a{2,1}`, `\`,
  `(?=x)` (lookahead), `a**`, `[]`, `[^]`, `(?P<1bad>x)`, `a{1001}` (>1000 cap), `)`, `[z-a]`, `**`.
- **Deep nesting** — 5000 nested `(...(a)...)` groups rejected (depth/group-count guard), no stack overflow
  under ASAN.
- Groups/anchors/classes/quantifiers/alternation/named-groups/`groupdict`/inline-flags/multiline-`^`/
  `\b`/non-capturing/`(?i)` unicode-ignorecase (`(?i)Ä` matches `ä`), DOTALL, `findall`/`split`/`sub`
  (string + function replacement) all correct, including empty-match semantics
  (`findall("a*","aaa")==['aaa','']`, `split("","abc")==['', 'a','b','c','']`, `sub("x*","-","abc")==
  "-a-b-c-"`). Verified against Python `re` behavior.

### Bytes (bytes.hpp)
- Construction/validation is strict: element out of `0..255` and negative counts throw; no silent
  `% 256` coercion. `encode`/`decode` correct for utf-8/latin-1/ascii; invalid UTF-8
  (`Bytes([0xff,0xfe]).decode("utf-8")`) throws; unknown encoding throws. Slice returns a `Bytes`
  (`type(b[1:4])=="Bytes"`, `b[::-1]` reverses). `hex`, `+`, `*`, `in` correct.

### Collections (collections.hpp)
- **Dict/Set insertion order** preserved across delete+reinsert (compact-layout tombstones).
- **Reentrancy guard (the key safety property) works and is ASAN-clean:** an `_eq_` that mutates the
  same Dict/Set *during a probe* (forced via a hash collision) is rejected with
  `Dict changed size during a key comparison`. A `_hash_` that mutates before the probe is intentionally
  allowed (runs before any entry reference is cached) and is memory-safe (no UAF under ASAN).
- **NaN keys** are write-only (`d[nan]=1; d[nan]=2` → `len==2`, `nan in d` → False) — documented, correct.
- **Unhashable keys** (`d[[1,2]]=1`) throw the standard error.
- **Set algebra** (union/intersection/symmetricdifference/issubset) correct; dedup order-independent.
- **Big-O ~linear:** Dict insert 0.5M/1M/2M = 251/515/1254 ms; Set = 596/1494/2567 ms (amortized linear,
  no quadratic blow-up on 2× input).
- **HashDoS resistance** (code inspection): bucket placement uses `bucketHash(seed)` with `seed` from the
  OS CSPRNG (`ObjectArena::makeHashSeed` → `randcompat::fillRandom`), overridable only via
  `KIRITO_HASH_SEED` for determinism; String/Bytes forgeable `hash()` is not used for placement.

### String methods (runtime.hpp)
- UTF-8 code-point (not byte) indexing/len throughout; case, strip (code-point matching so multibyte
  `chars` can't peel a continuation byte), startswith/endswith/find/rfind/index/count (incl. empty-substr
  boundary count and windowed `count("",1,2)==2`), split (sep + whitespace + maxsplit), join (streams a
  lazy source), replace (incl. empty-pattern interleave + `kMaxRepeat` guard), format (`{}`/`{{`/`}}`/
  indexed, huge-index → clean error), ljust/rjust/center/zfill (code-point width + byte-length cap),
  partition, removeprefix/suffix, is* predicates — all correct on empty/unicode/astral/windowed inputs.

### Lazy iterators (runtime.hpp)
- **Laziness / short-circuit confirmed:** `any(map(x==5, range(0,30_000_000)))` returns True in 0.03 ms
  (does not walk the range); an **infinite user `_next_` generator** short-circuits in `any(map(...))`.
- **Exceptions surface on consume, not create** (`map(1/x, [...0...])` — "created ok" then error on drain).
- **Re-iterable** map/filter (draining the same `map` object twice yields the same sequence — documented).
- zip (shortest), enumerate(start), filter all correct.

---

## Observations (documented design choices / low-severity — NOT defects)

1. **Simple 1:1 case mapping.** `"ß".upper()=="ß"` (Python: "SS") and `"İ".lower()=="ı"` (Python:
   "i̇"). Kirito does simple 1↔1 case mapping (ASCII+Latin-1+Latin-Ext-A+Greek), never the Unicode 1→N
   expansions. Documented ("handles ASCII + Latin-1 + Latin Extended-A"). SUSPECTED-not-a-bug: intended.

2. **`\w`/`\d`/`\s` are ASCII-only** in the regex engine, so `re.findall("\\w+","héllo")==['h','llo']`.
   Explicitly documented in `regex_engine.hpp` ("ASCII semantics, like most engines' default"). Intended.

3. **Snapshot iteration, no "changed size during iteration" error.** `for k in d: d[k+"z"]=0` and
   `for x in lst: lst.append(x)` iterate a *materialized snapshot* (Dict.keys()/List.elems vector) and do
   NOT raise the way CPython does. This is memory-safe (the snapshot is GC-rooted; ASAN-clean) — a
   deliberate semantic difference, not a bug. Worth a doc line if not already covered.

4. **`str.split()` (no-sep) uses `std::isspace` on bytes** (cast to `unsigned char`, so no UB), i.e.
   ASCII whitespace only — a Unicode whitespace like NBSP won't split. Consistent with `str.strip()`'s
   ASCII default. Minor Python-compat gap, not incorrect per Kirito's stated model.

5. **PERF (low): `MatchVal::groupString` rebuilds `utf8Starts(subject)` on every `.group()/.groups()/
   .groupdict()` call** (`stdlib_regex.hpp:79`). For a Match on a large subject, each group access is
   O(subject length), so `groups()` is O(subject × numGroups) and calling `.group()` across many
   finditer results is O(matches × subject). Correctness is fine; `findall`/`sub`/`split` already
   precompute `starts` once and are unaffected. SUSPECTED perf smell only — flag for a future pass, not a
   round blocker.

## Nulls / honesty
- I could not exhibit a single memory-safety or wrong-result defect in this scope. The reentrancy guards,
  UTF-8 validation-on-decode, regex work/size/depth caps, seeded hashing, and GC rooting in the lazy
  combinators are all present and effective on the real binaries.
- HashDoS resistance was confirmed by code inspection of the seed source, not by an end-to-end collision
  benchmark (bucket placement isn't observable from Kirito since insertion order is preserved regardless
  of seed).
