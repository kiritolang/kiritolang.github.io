# A5 — TEXT / COLLECTIONS · Deep Audit Report (scan3, v1.17.1)

Scope: String (UTF-8), Bytes, regex, List, Dict/Set, iterator protocol.
Files read in full: `builtins.hpp` (utf8 helpers, StrVal), `bytes.hpp`, `native.hpp`
(sliceIndices), `runtime.hpp` (StrVal methods, sequenceIndex, ListVal slice),
`stdlib_regex.hpp` surface. Binary: `build-bin/ki-asan`. Every probe below ran clean
under ASan/UBSan (no traces).

## Result: 0 CONFIRMED user-provable bugs (HIGH 0 · MED 0 · LOW 0)

This subsystem is exceptionally well-hardened. Extensive adversarial probing (UTF-8
boundaries, regex ReDoS, dup keys, mutation-during-iteration, Bytes coercion, huge
inputs, overflow) produced no reproducer that yields a wrong result, UB, or an asan
trace. Honest null.

---

## Informational (NOT counted — no user-provable bug; noted per brief)

### I1 · `builtins.hpp:223` · comment-vs-behavior mismatch (LOW, internal)
`BoolVal`'s comment says it "compares/keys equal to (True == 1)" and that
"`a == b => bucket(a) == bucket(b)` holds by construction". In reality Kirito treats
Bool as a distinct type: `True == 1` is **False** (verified), and `{True: 'a', 1: 'b'}`
keeps two keys. The behavior is internally consistent (strong typing; equality never
crosses Bool↔Integer, so the bucket invariant is vacuously satisfied), so this is only a
misleading code comment, not a defect. `True`/`1` still share a hash bucket but never
merge because `equals` differs — correct.
Reproducer: `io.print(True == 1)` → `False`; `io.print({True:"a", 1:"b"})` → `{True: 'a', 1: 'b'}`.

### I2 · `runtime.hpp:1651` · `center()` puts extra pad on the RIGHT (LOW, design)
`"xy".center(5)` → `" xy  "` (1 left, 2 right). CPython yields `"  xy "` (extra on the
left) for the same case. `docs/pages/09-types.md:174` only specifies "Center within
width `w`" — no side is contracted — so this is a defensible design choice, not a bug.
Deterministic and UTF-8-correct.

### I3 · mutation-during-iteration = snapshot semantics (design)
`for x in L: L.append(x)` terminates (iterates the 4 original elements, ends at len 8);
same for Dict/Set (`d["c"]=3` / `s.add(99)` during a loop do not crash or diverge).
Iteration eagerly materializes the source into a `vector<Handle>`, so mutation touches a
snapshot — memory-safe (no UAF under asan), deterministic. Differs from CPython, which
raises `RuntimeError` on dict/set resize during iteration. Worth a doc note, but not a
bug: no silent wrong result, no UB.

---

## Verified CORRECT (probed, not merely read)

UTF-8 String (`"héllo→世界"`, mixed 1/2/3-byte):
- `len` = 8; `s[1]`=`é`; `s[-1]`=`界`; `s[1:4]`=`éll`; `s[::-1]`=`界世→olléh` — code-point
  indexing/slicing never splits a multibyte char.
- `find`/`index`/`count` return **code-point** offsets: `"世界abc".find("界")`=1,
  `.find("abc")`=6; windowed `"→a→a→a".count("a",0,2)`=1, `.find("a",2)`=3 — start/end
  args are code points, not bytes.
- `upper`/`lower` correct for ASCII, Latin-1 and Latin Extended-A incl. Polish:
  `"łódź".upper()`=`ŁÓDŹ`, `"ąćęłńóśźż".upper()`=`ĄĆĘŁŃÓŚŹŻ` (round-trips). Ligature
  `"ﬃ".upper()` correctly left unchanged (out of mapped range).
- `strip("→")`, `split()` (whitespace), `split(",")` (empty fields kept), `replace("","-")`
  (boundary-interleaved), embedded NUL (`"a\x00b"` len 3, splits/finds correctly) — all correct.
- `sliceIndices` (native.hpp:54) overflow-guarded: `"abcdef"[0:6:10^12]`=`a`,
  `[::-10^18]`=`f` — no signed-overflow/UB; huge steps clamp to one element.
- Repeat cap: `"ab" * 9999999999999` → clean "repeat cap err"; negative repeat → `""`.

Bytes (strict, no lossy coercion):
- `Bytes([256])` / `Bytes([-1])` throw range errors — **no silent %256**.
- `Bytes([0xff,0xfe]).decode("utf-8")` throws (validUtf8 rejects) — no fabricated String.
- `"é".encode("ascii")` throws; latin-1 round-trips (`Bytes([200,201]).decode("latin-1")`=`ÈÉ`).
- Slicing `Bytes([1..6])[::2]`, `[::-1]`, `.hex()`=`0102fe` correct.

Regex (linear-time Thompson NFA):
- Classic ReDoS `(a+)+$` on 30 chars → 2 ms; `(a*)*b` on 5000 chars → 11 ms;
  `(a|a|aa)*b` on 40 chars → 1 ms — no catastrophic backtracking.
- Groups/offsets correct incl. multibyte: `re.search("世(界)", "hello世界!")` group 1 = `界`,
  start = 5 (code points). `group(5)` out-of-range throws.
- Empty-match safe (no infinite loop): `findall("a*","baaab")`=`['','aaa','','']`;
  `sub("","-","abc")`=`-a-b-c-`; `split("","abc")`=`['','a','b','c','']`.
- `IGNORECASE` folds multibyte (`É`~`é`), `MULTILINE` `^` works, backrefs in `sub`
  (`\2\1`) correct. Bounded quantifier capped: `a{1000000000}` → clean "repetition count
  too large (max 1000)" — no memory blowup.

Dict / Set:
- Duplicate literal keys: last wins (`{"a":1,"b":2,"a":3}` → `{'a':3,'b':2}`), len correct.
- Insertion order preserved; reassign keeps position; remove+reinsert moves to end.
- Integer/Float key equality merges (`d[1]` then `d[1.0]` → one entry); Bool/Integer stay
  distinct (design). Unhashable key (`d[[1,2]]`, `{[1,2]}`, set-as-key) throws cleanly.
- `union`/`intersection`/`symmetricdifference`/`difference` all correct.

Iterators:
- `map`/`filter` are lazy (0 calls before iteration) and short-circuit:
  `any(map(f,[1..5]))` evaluates only 3 of 5. Re-iterable without reentrancy crash
  (nested `for a in m: for b in m:` safe).
- No infinite lazy generators by design: `itertools.count(0)` without a stop bound errors
  explicitly ("no lazy generators") rather than hanging.

All probes clean under AddressSanitizer/UBSan.
