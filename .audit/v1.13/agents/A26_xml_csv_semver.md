# A26 — Audit: `xml` + `csv` + `semver` (frozen Kirito modules)

Agent: A26. Area: xml/csv/semver in `src/kirito/stdlib_kimodules.hpp`.
Method: STATIC reasoning only (no build/run). READ-ONLY — no source/test modified.
Source read: `stdlib_kimodules.hpp` csv (L638-724), xml (L2202-2497), semver (L2504-2876);
`runtime.hpp` `chr`/`isdigit`; tests `test_semver.cpp`, `test_xml.cpp`, `test_tabular_xml_deep.cpp`,
scripts `audit_csv.ki`, `audit_xml.ki`, `probe_xml.ki`, `spec_xml.ki`.

NOTE: `xml.parse` and `csv.parse` consume UNTRUSTED input; leniency (never crash) is their contract.

## Surface enumerated
- **csv**: `parse(text)`, `parserow(line)`, `format(rows)`, `formatrow(fields)`, `_needsQuote` (priv).
  RFC-4180-ish: doubled-quote escaping, embedded commas/newlines inside quotes, auto-quote on
  format.
- **xml**: `parse(text:String)`, `fromstring`(=parse alias), `tostring(element)`, class `Element`
  (`tag`/`attrib`/`text`/`tail`/`children`, `_iter_`/`_len_`/`_getitem_`, `get`/`find`/`findall`/
  `findtext`/`itertext`/`tostring`/`_str_`). Handles named+numeric entities, comments, CDATA,
  `<?xml?>`, `<!DOCTYPE/<! >`, single/double-quoted attrs. Parser + tostring + itertext all iterative
  (explicit work-stack) — robust to deep nesting. No DTD `<!ENTITY>` support ⇒ NOT vulnerable to
  billion-laughs (positive).
- **semver**: `clean`/`parse`/`valid`/`major`/`minor`/`patch`/`prerelease`; `compare`/`eq`/`neq`/`lt`/
  `lte`/`gt`/`gte`; `diff`/`inc`; range: `satisfies`/`validrange`/`maxsatisfying`/`minsatisfying`/
  `sort`/`rsort`; internal `_expand`/`_parserange`/`_testcomp`/`_cmpv`/`_cmppre`/`_cmpident`.

---
## CONFIRMED BUGS

### A26-1: XML surrogate numeric character reference crashes the "lenient" parser
- severity: **Medium** (untrusted input → uncaught exception; violates leniency contract)
- location: `stdlib_kimodules.hpp` `_parsehex` L2211-2228, `_parsedec` L2230-2240, `_decode` L2273-2274; vs `runtime.hpp` `chr` L2340-2342
- category: correctness / robustness / adversarial-crash
- description: `_parsehex`/`_parsedec` accept any code point 0..1114111 (0x10FFFF) and return it; they
  do NOT exclude the UTF-16 surrogate block 0xD800-0xDFFF. `_decode` then calls `chr(code)` for any
  `code >= 0`. But `chr` (runtime) **throws** `"chr argument is a UTF-16 surrogate (not a valid
  scalar code point)"` for cp in [0xD800, 0xDFFF]. The throw propagates out of `_decode` → out of
  `parse` (char-data path L2451 and attribute-value path L2419). The XML parser is documented and
  tested as "lenient — never crash on malformed input"; a surrogate entity breaks that.
- failure-scenario: `xml.parse("<x>&#xD800;</x>")` or `xml.parse("<x>&#55296;</x>")` or in an
  attribute `xml.parse("<a t='&#xDC00;'/>")` throws an uncaught KiritoError instead of decoding
  leniently (e.g. keeping verbatim). Hostile XML can crash any consumer that relied on the parser
  being total.
- proposed-test: add to `spec_xml.ki`/`audit_xml.ki`: `assert safe(Function(): return
  xml.parse("<x>&#xD800;</x>"))` and a surrogate in an attribute; assert it does not throw.
- proposed-fix: in `_parsehex`/`_parsedec` return -1 when `0xD800 <= v <= 0xDFFF` (treat as
  malformed → kept verbatim), OR wrap the `chr(code)` call in `_decode` in a try that falls back to
  verbatim. Prefer rejecting surrogates in the numeric parsers (matches XML: surrogates are not legal
  char refs).
- confidence: **high** (both sides of the mismatch verified in source).

### A26-2: semver partial `>` / `<=` comparators diverge from node-semver (wrong satisfies)
- severity: **Medium** (wrong version resolution for `kpm repo@<constraint>`; documented as
  "node-semver-style")
- location: `_expand` explicit-comparator fallthrough L2743-2744 (`return [{"op": op, "v":
  _mkver(mj, mn, pt, prepart)}]`)
- category: correctness / range-grammar divergence
- description: For a comparator with a PARTIAL version, node-semver rounds the bound: `>1` → `>=2.0.0`,
  `>1.2` → `>=1.3.0`, `<=1.2` → `<1.3.0`, `<=1` → `<2.0.0`. Kirito instead fills missing components
  with 0 for EVERY operator, so `>1` becomes `>1.0.0`, `<=1.2` becomes `<=1.2.0`, `<=1` becomes
  `<=1.0.0`. (`>=` and `<` partials fill-with-0 correctly and DO match node-semver — only the
  round-UP cases `>` and `<=` are wrong.)
- failure-scenario: `satisfies("1.5.0", ">1")` → Kirito True (via `>1.0.0`), node-semver **False**
  (via `>=2.0.0`). `satisfies("1.5.0", "<=1")` → Kirito False (via `<=1.0.0`), node-semver **True**
  (via `<2.0.0`). `satisfies("1.4.0", "<=1.2")` → Kirito False, node-semver **True**. A user writing
  `kpm install repo@">1"` (meaning "2.x+") gets a 1.x tag resolved.
- proposed-test: `test_semver.cpp`: `CHECK(sat("1.5.0", ">1") == "False")`,
  `CHECK(sat("2.0.0", ">1") == "True")`, `CHECK(sat("1.5.0", "<=1") == "True")`,
  `CHECK(sat("1.4.0", "<=1.2") == "True")`.
- proposed-fix: in `_expand`, for op `>` with a missing/x minor or patch, emit `>=` with the next
  incremented significant component (mirror node-semver's replaceXRange); for op `<=` with a partial
  version, emit `<` with the incremented component. `>=`/`<` partials already fill-with-0 correctly.
- confidence: **high** (node-semver partial-comparator rounding is well specified; Kirito clearly
  fills-with-0 unconditionally).

---
## DIVERGENCES / VALIDATION GAPS

### A26-3: semver `inc` on a prerelease always bumps (diverges from node-semver)
- severity: Low-Medium (documented simplification, but "à la node-semver"; a test enshrines it)
- location: `inc` L2639-2655
- category: correctness / node-semver divergence
- description: node-semver treats a prerelease as "already heading toward" its base and only DROPS the
  prerelease when the target segment is already 0: `inc("1.2.3-rc.1","patch")` → `"1.2.3"`;
  `inc("1.2.0-rc.1","minor")` → `"1.2.0"`; `inc("2.0.0-rc.1","major")` → `"2.0.0"`. Kirito
  unconditionally increments then drops prerelease: those give `"1.2.4"`, `"1.3.0"`, `"3.0.0"`. The
  existing test `CHECK(ev(... inc("1.2.3-rc.1","patch")) == "1.2.4")` locks in the divergent result.
- failure-scenario: any release-flow that increments a prerelease toward its final version gets a
  version one step too high.
- proposed-test: document/decide intended semantics; if node-semver parity is wanted, add cases for
  the three prerelease-at-0 forms above.
- proposed-fix: (if parity desired) when `p["prerelease"]` non-empty and the target segment is 0
  (patch for "patch"; patch==0 for "minor"; minor==0 && patch==0 for "major"), drop prerelease WITHOUT
  incrementing. Also `inc` lacks node-semver's `premajor`/`preminor`/`prepatch`/`prerelease` release
  types (throws) — a feature gap, not a bug.
- confidence: high on divergence; the "bug vs deliberate" call is a design decision.

### A26-4: semver `parse`/`valid` accept illegal identifiers (no [0-9A-Za-z-] check, leading zeros)
- severity: Low (over-lenient validation)
- location: `parse` L2521-2544 (only checks `len(p)==0` for prerelease; `_isnum` for core)
- category: correctness / validation gap
- description: The comment claims prerelease identifiers "must be non-empty [0-9A-Za-z-]" but only the
  non-empty check is enforced. So `valid("1.2.3-!!!")`, `valid("1.2.3-a b")` (space), and build with
  illegal chars are accepted. Leading zeros are not rejected: core `01.2.3` (`_isnum("01")` true →
  `Integer("01")`=1) and numeric prerelease `1.2.3-01` are accepted though semver.org forbids
  leading-zero numeric identifiers. `_cmpident` treats `"01"` as numeric so `1.2.3-01 == 1.2.3-1`.
- failure-scenario: `valid("01.02.03")` returns the raw string (should be None); two differently-
  spelled versions compare equal; kpm may accept a malformed tag.
- proposed-test: `CHECK(ev("s.valid(\"01.2.3\")") == "None")`,
  `CHECK(ev("s.valid(\"1.2.3-01\")") == "None")`, `CHECK(ev("s.valid(\"1.2.3-a!b\")") == "None")`.
- proposed-fix: validate each core component has no leading zero (unless "0"); validate each
  prerelease/build id matches `[0-9A-Za-z-]+` and numeric ids have no leading zero.
- confidence: high (validation logic is fully visible).

### A26-5: XML multiple top-level elements — only the LAST is returned (silent data loss)
- severity: Low (single-root assumption; not a crash)
- location: `parse` L2484-2487 (`else: root = elem` on an empty stack)
- category: correctness / edge
- description: When the stack is empty and a new start tag appears, `root` is reassigned. For input
  with two or more top-level elements (`"<a/><b/>"`, or content after the first tree closes), each new
  top-level element OVERWRITES `root`; earlier ones are discarded. Character data between top-level
  elements is dropped (`if len(stack) > 0` guard). ElementTree raises on multiple roots; Kirito
  silently keeps the last.
- failure-scenario: `xml.parse("<a>1</a><b>2</b>").tag` → "b" (the `<a>` tree is lost).
- proposed-test: assert documented behavior explicitly, or (if changed) collect/forbid multi-root.
- proposed-fix: document as "returns the last top-level element"; or keep the FIRST root and ignore
  later top-level tags; or throw. At minimum add a test pinning the behavior.
- confidence: high.

---
## WEAK SPOTS (hostile input: perf / mis-parse, not crash)

### A26-6: O(n²) character-by-character string building on untrusted input (DoS-ish)
- severity: Low-Medium (algorithmic; untrusted input)
- location: csv `parse`/`parserow` `current = current + c` (L667/674/699/704/718 etc.);
  xml `_decode` `out = out + ...` per char (L2246-2282); xml `Element.tostring` `out = out + item[1]`
  (L2361); `_addtext` repeated `text = text + data` (L2435/2437)
- category: performance / resource-exhaustion
- description: Kirito Strings are immutable, so `s = s + c` in a loop copies the whole accumulator →
  O(n²). CSV builds every field one char at a time unconditionally, so a single huge field (e.g. one
  10 MB quoted cell, or one giant line) is quadratic. `_decode` is O(m²) for any text chunk containing
  `&`. `tostring` concatenates fragments into `out`, O(total²) for a large tree. Additionally
  `_decode`'s per-`&` `s.find(";", i)` is O(n) each, so a chunk of many `&` with a distant `;` is
  O(n²) scanning on top of the O(n²) build.
- failure-scenario: a multi-megabyte CSV field or an XML text node with many `&` makes parse hang /
  burn CPU (quadratic) — an untrusted-input soft-DoS.
- proposed-test: (perf, hard to assert) — at least a moderately-large-field parse in the suite to
  catch a regression to something worse.
- proposed-fix: accumulate fields/text as a List of chunks + `"".join` at the end (slice whole runs
  instead of per-char where possible; csv could `find` the next delimiter and slice). Language-wide
  pattern; note as a systemic weak spot, not unique to these modules.
- confidence: high on the complexity; severity depends on exposure.

### A26-7: CSV lone-CR (old-Mac) and CR-in-field handling is lossy / inconsistent
- severity: Low
- location: csv `parse` `elif c == "\r": pass` L715-716 (only in `parse`, not `parserow`)
- category: correctness / edge
- description: `parse` silently DROPS every bare `\r` outside quotes (to fold CRLF→LF). Consequences:
  (a) a lone-CR line-ending file (classic Mac) is NOT split into rows — the whole file collapses to
  one row; (b) a legitimate `\r` in unquoted field data is dropped; (c) `parserow` has no `\r` case at
  all, so it KEEPS `\r` — `parse` and `parserow` disagree on identical field content. Also `format`
  joins rows with `"\n"` (LF) only, so output is not RFC-4180 CRLF.
- failure-scenario: `csv.parse("a\rb\rc")` → one row `["abc"]` (CRs dropped) instead of 3 rows;
  `csv.parserow("a\rb")` → `["a\rb"]` while the same content via `parse` loses the `\r`.
- proposed-test: `csv.parse("a\rb")`, `csv.parserow("a\rb")`, a CRLF file, and a field containing a
  literal CR — pin the intended behavior.
- proposed-fix: decide policy: treat lone `\r` and `\r\n` both as row separators outside quotes
  (normalize like the lexer does), and preserve `\r` only inside quotes; make `parserow` consistent.
- confidence: high (behavior fully visible).

### A26-8: XML `<!DOCTYPE ... [ internal subset ]>` mis-parsed (stops at first `>`)
- severity: Low (lenient, no crash)
- location: `parse` `<!` branch L2470-2473 (`close = text.find(">", i)`)
- category: correctness / edge
- description: The `<!` handler skips to the FIRST `>`. A DOCTYPE with an internal subset
  `<!DOCTYPE x [ <!ENTITY foo "bar"> ]>` contains `>` inside the subset, so the parser resumes in the
  middle of the subset and treats the remainder (`]>` and following) as text/markup. No crash, but the
  declaration is not fully skipped. (Positive side: because `<!ENTITY>` is never honored, there is no
  custom-entity expansion and thus NO billion-laughs vulnerability.)
- failure-scenario: `xml.parse("<!DOCTYPE x [<!ENTITY e 'v'>]><r/>")` may not return `<r/>` as root
  cleanly (leftover `]>` handled as char data at top level, then `<r/>` becomes root — actually still
  fine here, but nested `>` in an internal subset generally derails the skip).
- proposed-test: add a DOCTYPE-with-internal-subset input to the malformed set; assert no crash and
  the following root element is found.
- proposed-fix: for `<!DOCTYPE` specifically, if a `[` precedes the first `>`, skip to `]>`.
- confidence: medium (edge; effect depends on exact content).

---
## MINOR / DRY / LOW

### A26-9: DRY — csv `parse` and `parserow` duplicate the quote state machine
- severity: Low (maintainability)
- location: csv `parserow` L651-677 vs `parse` L685-723
- category: DRY
- description: `parserow` is exactly `parse`'s inner loop minus the `\n`/`\r` row handling. The quote-
  toggle + doubled-quote-escape logic is copied verbatim; a fix to one (e.g. A26-7) must be mirrored.
- proposed-fix: factor a single field-scanner, or implement `parserow(line)` as `parse(line)[0]` (with
  care for the empty-line case). Also semver `compare` (L2597-2609) and `_cmpv` (L2664-2674) duplicate
  the major/minor/patch/prerelease comparison chain — `compare` could parse then call `_cmpv`.
- confidence: high.

### A26-10: XML unquoted attribute value silently becomes "" and name-splits oddly
- severity: Low (lenient behavior, undocumented)
- location: `_parse_tag` L2408-2421 (no branch for an unquoted value after `=`)
- category: correctness / edge
- description: For `<a x=unquoted>`, after `x=` the next char is not a quote, so `aval` stays "" and
  the scanner then reads `unquoted` as a SECOND attribute name (value ""). Result:
  `{"x": "", "unquoted": ""}`. Lenient, no crash, but surprising; probe_xml only asserts "does not
  crash". HTML-style unquoted attributes are silently mangled.
- proposed-test: pin `xml.parse("<a x=1>...").attrib` behavior explicitly.
- proposed-fix: (optional) support unquoted attribute values (read until whitespace/`>`/`/`).
- confidence: high.

### A26-11: semver `parse`/`compare` may throw on a >int64 numeric core (uncaught in hot path)
- severity: Low (bounded by valid()/satisfies() guards in the common paths)
- location: `parse` L2543 `Integer(nums[0..2])`; `compare` L2597-2599 (no try)
- category: robustness / edge
- description: `_isnum` passes an arbitrarily long all-digit component; `Integer("999...")` (>int64)
  may throw or wrap. `valid`/`satisfies`(range side)/`maxsatisfying` wrap parse in try or guard with
  `valid()`, so those are safe. But `compare(a,b)` and bare `parse(s)` on an overflowing-but-all-digit
  version throw a low-level conversion error rather than a clean "invalid version". `satisfies`'s
  `parse(version)` at L2813 is OUTSIDE its try (only the range parse is guarded), so a malformed
  `version` argument there also throws.
- proposed-test: `CHECK_THROWS`/behavior for `compare("99999999999999999999.0.0","1.0.0")` and
  `satisfies("99999999999999999999.0.0","*")`.
- proposed-fix: bound component length in `parse` (semver has no fixed cap, but int64 is the storage);
  and/or wrap `parse(version)` in `satisfies` so a bad version → False rather than throw.
- confidence: medium (depends on Integer(str) overflow semantics, which I did not execute).

---
## COVERAGE ASSESSMENT (tested vs gaps)

Well covered: csv typical/quoting/embedded-newline/adversarial-unmatched-quote/unicode/round-trip
(audit_csv). xml structure/attrs/entities(named+dec+hex BMP)/CDATA/comments/decl/DOCTYPE/mixed-content/
itertext/serialize round-trip/deep-nesting(2000-3000, iterative)/fuzz(400+200 trees)/malformed-no-crash.
semver parse/valid/precedence(spec chain)/caret/tilde/x-range/hyphen/OR/comparators/AND/prerelease
gating/maxsatisfying/sort/inc/diff/fuzz(3000 + ordering invariants).

Notable GAPS (no test):
- **xml**: surrogate numeric entity (A26-1 crash); `&#0;`/control chars; astral char ref
  (`&#x1F600;`, only BMP snowman tested); empty numeric `&#;`; DOCTYPE internal subset (A26-8);
  multiple top-level elements (A26-5); unquoted attribute value semantics (A26-10); attribute value
  containing `<`/`>`; unterminated attribute quote.
- **csv**: lone-CR / CRLF / CR-in-field (A26-7); `parse`↔`parserow` consistency; format→parse
  round-trip of a field with an embedded newline; a very large single field (A26-6 perf).
- **semver**: partial `>`/`<=` comparators (A26-2 bug); leading-zero rejection & illegal id chars
  (A26-4); `inc` on prerelease-at-0 (A26-3); hyphen with a partial LOWER bound (`1 - 2`); overflow
  numeric core (A26-11); `premajor`/etc. inc types (unsupported/throws).

## POSITIVE NOTES (verified robust)
- xml parse/tostring/itertext are iterative (work-stack) → deep nesting does NOT overflow (tested 3k).
- No DTD `<!ENTITY>` support → no billion-laughs / entity-expansion bomb.
- Numeric-entity accumulators cap each step at ≤1114111 → no int64 overflow in `_parsehex`/`_parsedec`.
- `isdigit` is ASCII-only → semver `_isnum` cannot feed a Unicode "digit" into `Integer()` (safe).
- semver caret/tilde/x-range/hyphen/prerelease-gating logic matches node-semver on the tested cases.
