# A11 — fuzzing + adversarial input (whole interpreter), round v1.15.1

## Scope

Try to break `./build-debug/ki`. Contract under test: **for ANY input, `ki` either runs it or fails
with a clean, catchable Kirito diagnostic. Never segfault, abort, hang, corrupt a value, or leak a
raw C++ exception.**

Areas:
1. Source fuzz — random bytes/tokens, corpus mutation, nesting, encodings, unterminated everything.
2. Blob fuzz — `dump.loads`/`serialize.loads` on random + bit-flipped valid blobs; `json`, `zlib`,
   `gzip`, `base64`, `regex`, `Bytes.fromhex`, `int.fromstring`, `time.strptime`, `xml`, `csv`,
   `semver`.
3. Value fuzz — every binary op × every type pair; random method calls; cycles through
   `str`/`len`/`sorted`/`==`/`dump`/`json`.
4. Resource guards — huge repetition/padding/range, deep recursion, deep nesting (bounded via
   `ulimit -v` + `timeout`).
5. `--gc-threshold 1` differential over a corpus slice (the A19-2 shape).

Rule: **no builds**. Harnesses are Python/bash driving the prebuilt binary.

---

## Log

(started; appending as I go)

---

### A11-1: `xml` entity decoding is O(n²) — one `&` in a text node makes parsing 500× slower  [severity: MED] [confidence: confirmed]

- **Location**: `src/kirito/stdlib_kimodules.hpp:2323-2363` (`xml`'s `_decode`)
- **What**: `_decode` accumulates its result with `out = out + s[i]` **one code point at a time**.
  Kirito strings are immutable, so each `+` copies the whole accumulator — classic O(n²). There is a
  fast path (`if "&" not in s: return s`), so plain text is fine, but **a single `&` anywhere in a
  text node drops the ENTIRE node onto the char-by-char path**. An XML parser's threat model is
  untrusted input, so this is a genuine availability bug, not a micro-optimisation.

- **Repro** (found by the blob fuzzer as a >25 s timeout on
  `xml.fromstring("<a>&amp;" * 10000 + "</a>")`, then minimised to a single entity):

```kirito
var io = import("io")
var t = import("time")
var s = "<a>" + "x" * 40000 + "&amp;</a>"   # 40 KB of plain text, ONE entity
var t0 = t.monotonic()
discard import("xml").fromstring(s)
io.print(Integer((t.monotonic() - t0) * 1000), "ms")
```

Real output — the `&` costs 500×:

```
textlen 5000  -> 107 ms
textlen 10000 -> 381 ms      (2x input -> 3.6x time)
textlen 20000 -> 1358 ms
textlen 40000 -> 5047 ms
40000 chars, NO '&': 10 ms   <-- the fast path
```

Entity-dense input scales the same way (`"<a>" + "&amp;"*n + "</a>"`): 200→12 ms, 400→31, 800→100,
1600→357, 3200→1353 — ~4× per doubling, i.e. O(n²). Extrapolated, a 400 KB text node containing one
ampersand takes ~8 minutes. No crash, no wrong value — it just hangs.

- **Impact**: anyone parsing untrusted/large XML (RSS/Atom via `feedreader`, HTTP payloads). A ~1 MB
  document with one `&` is an effective DoS. `&` is extremely common in real XML (URLs in feeds).
- **Proposed fix**: accumulate into a List of chunks and `"".join(...)` at the end, appending **runs**
  of plain text instead of single characters — i.e. use `s.find("&", i)` to copy the whole span up to
  the next entity in one slice. Both changes are needed: chunking fixes the accumulator, run-copying
  fixes the per-char loop. No contract moves — output is byte-identical, only the cost changes.
- **Proposed test**: `tools/tests/scripts/` — assert a 40 KB text node with one entity parses well
  under a time bound, plus value-equality assertions that the entity semantics (`&lt;`/`&amp;`/
  numeric/unknown-verbatim) are unchanged. See "Proposed permanent tests" below.

---

### A11-2: `base64.encode` is O(n²) — encoding 32 KB takes 5.7 s, while `decode` of the same data is linear  [severity: MED] [confidence: confirmed]

- **Location**: `src/kirito/stdlib_kimodules.hpp:628-637` (`base64`'s `encode`)
- **What**: same defect class as A11-1 and found by chasing it: `encode` builds its output with
  `out = out + <char>`, four immutable-string copies per input triple → O(n²). **`decode`, twelve
  lines below it in the same module, is correct** — it appends to a List (`out.append(...)`). So the
  module contains both the right and the wrong idiom; only one path is slow. `csv.formatrow` in the
  next module also does it correctly (`parts` + `",".join(parts)`), which is the established
  house idiom.

- **Repro**:

```kirito
var io = import("io")
var t = import("time")
var t0 = t.monotonic()
discard import("base64").encode("A" * 32000)
io.print(Integer((t.monotonic() - t0) * 1000), "ms")
```

Real output — encode is quadratic, decode is linear:

```
=== base64.encode scaling ===          === base64.decode scaling ===
2000  ->   36 ms                       2000  ->  18 ms
4000  ->  116 ms   (3.2x)              8000  ->  89 ms   (4x in, 4.9x time)
8000  ->  419 ms   (3.6x)              32000 -> 338 ms   (4x in, 3.8x time)  <- LINEAR
16000 -> 1516 ms   (3.6x)
32000 -> 5683 ms   (3.7x)  <- ~4x per doubling = O(n^2)
```

- **Impact**: worse in practice than A11-1, because base64-encoding a ~1 MB payload (a file, an
  image, an HTTP body, an email attachment) is an everyday operation. Extrapolating the measured
  curve, 1 MB takes on the order of **an hour**. Anything using `base64.urlsafeencode` (which calls
  `encode`) inherits it.
- **Proposed fix**: accumulate into a List and `"".join(...)` — i.e. exactly what `decode` already
  does. Output is byte-identical; no contract moves. (Note the documented false-positive that
  `base64.decode` rejects embedded whitespace — that is a *decode* behaviour and is untouched by
  this.)
- **Proposed test**: `tools/tests/scripts/` — a size-scaling bound plus a round-trip equality
  assertion. See "Proposed permanent tests" below.
