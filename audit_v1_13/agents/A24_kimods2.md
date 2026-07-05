# A24 — ki-authored modules subset 2 (statistics, string, textwrap, tee, arg, base64)

Audit agent: **A24** | Area: `src/kirito/stdlib_kimodules.hpp` (statistics, string, textwrap, tee, arg, base64)
Method: static reasoning only, READ-ONLY. No source/test modified. No build/run.

Tests reviewed: `tools/tests/unit/test_stdmodules.cpp`, `test_stdlib_extra.cpp`, `tools/tests/scripts/`.

---

## Findings

### A24-1: statistics.mean/variance/stdev overflow on large-integer data (sums int64 before Float)
- severity: low
- location: `stdlib_kimodules.hpp:346-349` (`mean`); propagates to `variance:378`, `stdev:395`.
- category: correctness / numerical stability
- description: `mean` returns `Float(sum(data)) / Float(len(data))`. `sum(data)` accumulates in **int64** for an all-Integer list, so it wraps (well-defined two's-complement) BEFORE the `Float()` conversion. `median` avoids this by converting each element to Float first (`Float(s[i])`), but `mean` does not — and `variance`/`stdev`/`pstdev` call `mean`, so they inherit the defect.
- failure-scenario: `mean([5000000000000000000, 5000000000000000000])` — true mean 5e18, but `sum` wraps to ≈ -8.4e18, so `mean` returns a large negative Float silently. Python (arbitrary-precision ints) would be correct.
- proposed-test: `assert st.mean([4611686018427387904, 4611686018427387904]) == 4611686018427387904.0` (2^62 each; sum = 2^63 overflows int64).
- proposed-fix: sum as Float — `var total = 0.0; for x in data: total = total + Float(x)` — or document the int64-range limitation.
- confidence: medium (depends on `sum` builtin returning wrapping Integer for integer lists, which matches documented int64 semantics).

### A24-2: base64.decode/encode reject/garble whitespace and out-of-range bytes (MIME robustness)
- severity: low
- location: `stdlib_kimodules.hpp:604-627` (`decode`), `571-602` (`encode`).
- category: robustness / interop gap
- description: (a) `decode` throws `invalid base64 character` on any char not in the 64-char alphabet — including `\n`/`\r`/space. Real-world/MIME base64 (PEM, HTTP, email) is line-wrapped at 76 cols with embedded newlines; Python's `b64decode` (validate=False) skips them. Here they throw. (b) `encode` does no range/type validation of List elements: a value >255 or <0 silently produces wrong output (`(triple//…)%64` masks the index into range, no crash), and a non-Integer element throws a raw arithmetic type error, not a clear message.
- failure-scenario: `b64.decode("TWFu\nTWFu")` throws instead of decoding; `b64.encode([256])` silently returns garbage; `b64.encode(["x"])` throws an opaque error.
- proposed-test: assert a newline-wrapped base64 blob decodes (if leniency is desired), or assert-throws with a documented message; assert `encode` validates 0..255.
- proposed-fix: strip ASCII whitespace in `decode` before the loop; range-check bytes in `encode` with a clear throw.
- confidence: high (behavior is directly readable; leniency-vs-strict is a design call, so filed low).

### A24-3: base64.decode of a Bytes argument crashes with an opaque error
- severity: info
- location: `stdlib_kimodules.hpp:609-613`.
- category: robustness / API asymmetry
- description: `decode(s)` iterates `for ch in s` assuming `s` is a String (each `ch` a 1-char String). If a caller passes a **Bytes** (a natural mistake, since `encode` accepts Bytes and binary I/O yields Bytes), iteration yields Integers; `ch == "="` is always False and `ch not in _index` is True, so it hits `throw "... '" + ch + "'"` — `String + Integer` — an opaque type error rather than a clear "expected a String" message.
- failure-scenario: `b64.decode(b64.encode(Bytes([1,2,3])).encode())`.
- proposed-test: assert-throws (with a readable message) when `decode` is given Bytes.
- proposed-fix: at the top of `decode`, if `type(s) == "Bytes"` decode it (or throw a clear message).
- confidence: high.

### A24-4: statistics.mean/variance give opaque errors on non-numeric / mixed data
- severity: info
- location: `stdlib_kimodules.hpp:346-349`, `374-382`.
- category: robustness (gap)
- description: `mean(["a","b"])` throws from inside `sum` (or `Float`), not a domain-specific message; `variance` of strings throws at `Float(x)`. No graceful "requires numeric data" diagnostic. Not tested.
- proposed-test: assert-throws for `mean(["a"])`, `variance(["a","b"])`.
- proposed-fix: optional type check with a clear message (or accept the underlying error as documented).
- confidence: high.

### A24-5: string.similarity(a, b) with b neither String nor List errors opaquely
- severity: info
- location: `stdlib_kimodules.hpp:469-478`.
- category: robustness (gap)
- description: `similarity` branches on `type(b) == "List"`, else assumes `b` is a String and calls `a.levenshtein(b)`. If `b` is an Integer/None/etc., the native `levenshtein` throws an opaque error. `closest`/`fuzzymatch` similarly assume `candidates` is a List of Strings (native levenshtein error otherwise).
- proposed-test: assert-throws (readable) for `similarity("a", 5)`.
- proposed-fix: none required if documented; otherwise validate argument type.
- confidence: high.

### A24-6: string.capwords diverges from Python on tabs / repeated spaces
- severity: info
- location: `stdlib_kimodules.hpp:453-460`.
- category: correctness divergence (gap)
- description: `capwords` splits/join on a single `" "` only. Python's `str.capwords` (sep=None) splits on arbitrary whitespace and **collapses** runs. Here `capwords("a  b")` → `"A  B"` (double space preserved), and tab/newline-separated words are not word-boundaries (`capwords("a\tb")` → `"A\tb"` — only the first token capitalized). Not tested.
- proposed-test: `assert strm.capwords("a  b\tc") == ...` documenting the chosen behavior.
- proposed-fix: split on whitespace and collapse if Python-parity is intended; else document.
- confidence: high.

### A24-7: textwrap.wrap does not treat width<=0 or embedded newlines specially
- severity: info
- location: `stdlib_kimodules.hpp:512-526`.
- category: robustness / correctness divergence (gap)
- description: (a) `width <= 0`: the first word is emitted regardless (`len(current)==0` branch), then every subsequent word lands on its own line — no crash, **no hang**, but Python raises `ValueError` for width<=0. Not tested. (b) Newlines/tabs inside `text` are not whitespace to `split(" ")`, so `wrap("a\nb c", 10)` yields `["a\nb", "c"]` (a literal `\n` embedded); Python collapses all whitespace. Long-word non-breaking IS tested (deep_kimods:228); width<=0 and embedded-newline are not.
- proposed-test: `assert tw.wrap("hi there", 0) == ["hi", "there"]` (pin current behavior); test with `\n`/`\t` in input.
- proposed-fix: document, or normalize whitespace / guard width<=0.
- confidence: high.

### A24-8: textwrap.dedent leaves partial indent on whitespace-only lines (Python normalizes to empty)
- severity: info
- location: `stdlib_kimodules.hpp:541-558`.
- category: correctness divergence (gap)
- description: Whitespace-only lines are (correctly) ignored when measuring the common indent, but in the second pass a whitespace-only line with `len(line) >= minIndent` is sliced rather than emptied. `dedent("  a\n   \n  b")` → `"a\n \nb"` (a stray space survives); Python's `textwrap.dedent` normalizes whitespace-only lines to empty. deep_kimods tests blank-line ignoring (`"    a\n\n    b"`) but not a whitespace-only interior line longer than the common indent.
- proposed-test: `assert tw.dedent("  a\n   \n  b") == "a\n\nb"` (Python parity) or pin the current `"a\n \nb"`.
- proposed-fix: emit `""` for whitespace-only lines in the second pass.
- confidence: high.

### A24-9: arg — short-option and inline-`=` code paths largely untested; several silent-mishandling edges
- severity: info
- location: `stdlib_kimodules.hpp:1117-1140`, `1179-1215`.
- category: coverage gap + minor robustness
- description: `r9_stdlib_d.ki` covers `--opt val`, flags, defaults, `rest`, `-h/--help`, unknown option, missing positional, String option. **Untested / edge behaviors:**
  - Short options `-x` (single letter, via `_byshort`) — never exercised.
  - `--opt=val` inline form (`1183-1186`) — never exercised.
  - Option missing its value at end of args (`--count` last) → `throw "... requires a value"` (`1194`, `1211`) — untested.
  - Type-convert failure: `--count abc` with an Integer default → `throw "option --count expects an integer..."` (`1133`, `1138`) — untested.
  - `-abc` (multi-char after single `-`) fails the `len(token)==2` guard (`1199`) and is silently swallowed as a **positional**, not flagged as an unknown option.
  - `--flag=value` sets the flag True and **silently ignores** the `=value`.
  - `_byshort` returns the **first** flag/option whose name starts with the letter — two names sharing a first letter resolve ambiguously with no warning; flags are checked before options.
  - `--` alone throws `unknown option: --` rather than acting as an end-of-options separator (Python convention).
  - A positional literally named `"rest"` collides with the auto-injected `result["rest"]`.
- failure-scenario: a CLI declaring `-v`/`--verbose` never sees `-v` tested; `prog --count x` error message untested; `prog -xy` silently treated as a positional.
- proposed-test: add cases for `-x` short option, `--opt=val`, missing-value throw, bad-numeric throw, and pin the `-abc`→positional behavior.
- proposed-fix: none required for correctness; optionally reject unknown `-abc`, and validate option-name uniqueness.
- confidence: high.

### A24-10: tee — copy-stream write failures and non-flushable primaries are only partly guarded
- severity: info
- location: `stdlib_kimodules.hpp:1017-1044`.
- category: robustness (gap)
- description: `Tee.write` calls `s.write(data)` on each copy with no guard — a copy stream lacking `write` (or a closed file) throws mid-fan-out, and (since copies are written *before* the primary) the primary then never receives the chunk. `flush`/`_maybeFlush` correctly swallow flush failures, but `write` does not. `Tee.write` returns `len(data)` (code points for a String, bytes for Bytes) — fine, but not asserted. spec_tee/r9 cover the happy path + restore-on-throw; a broken copy stream is untested.
- proposed-test: `Tee` with a copy object that has no `write` → assert-throws (documenting it), and assert `write` returns the length.
- proposed-fix: optional — document that copy streams must implement `write`.
- confidence: medium.

### A24-11: statistics.quantiles offers only the exclusive method (no `method=` / inclusive); n/method edges
- severity: info
- location: `stdlib_kimodules.hpp:418-439`.
- category: coverage / feature gap
- description: Only the exclusive method is implemented (documented). Python's `quantiles` also has `method="inclusive"`. Interior interpolation, both clamp arms, sort-independence, and `n<2`/`n<1` throws ARE tested (r8_kimods_a). Not covered: very large `n` relative to data (mostly-clamped output), Float data with ties, and the `Integer(pos)` truncation at an exact-integer `pos` (frac==0 interior path). Low value; noting for completeness.
- proposed-test: `quantiles([1,2,3,4,5], 100)` shape/clamp behavior; exact-integer position (`quantiles([1,2,3,4], 2)` → single median cut).
- proposed-fix: none (documented scope).
- confidence: high.

### A24-12: DRY — levenshtein correctly reused; no statistics/math duplication (positive)
- severity: info
- location: `stdlib_kimodules.hpp:466-507` (string), `344-398` (statistics).
- category: DRY (observation, no action)
- description: The fuzzy helpers (`similarity`/`closest`/`fuzzymatch`) all delegate to the **native** `String.levenshtein` (single native call over a List) — no reimplemented edit-distance in Kirito; good reuse. `statistics` imports `math` only for `sqrt` (`stdev`/`pstdev`) and does not duplicate math internals. One minor local duplication: the ratio formula `1.0 - dist/longer` is factored into `_ratio` and used by `similarity`, but `fuzzymatch` (`501-502`) inlines the same computation instead of calling `_ratio` — a small, harmless copy that could call `_ratio`.
- proposed-fix: have `fuzzymatch` call `_ratio(len(query), dists[i], len(candidates[i]))`.
- confidence: high.
