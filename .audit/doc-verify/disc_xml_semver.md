# doc-verify: `xml` + `semver` stdlib modules

Impl: `src/kirito/stdlib_kimodules.hpp` (Kirito-authored, frozen-source; xml @ ~L2243, semver @ ~L2548).
Docs: `docs/pages/10-stdlib.md` (`## xml`, `## semver`), `docs/pages/04-packages.md` (kpm range grammar).
Tests: `tools/tests/scripts/verify_xml.ki` (+`.expected` `OK xml`), `verify_semver.ki` (+`.expected` `OK semver`).
Runner: `build-debug/ki`. Both files pass, exit 0.

## Verdict: NO genuine defects, NO doc/impl mismatches.

Docs match the implementation for every documented function/method/attribute. The parser's lenient
"never crash" contract holds on all odd inputs probed (including the surrogate-entity fix). No `src/`
change warranted or made.

## Behaviour PINNED by the tests (exact, so a regression is caught)

### xml
- `parse`/`fromstring` (alias): navigate `tag`/`attrib`/`text`/`tail`/`children`; `find`/`findall`/
  `findtext`/`get`/`itertext`; iteration yields children; `len`/`elem[i]` indexing.
- `tostring` / `_str_`: mixed-content round-trips (`<a>t1<b>x</b>tail1<c />tail2</a>`); empty elements
  self-close as `<b />` (note the space); text/attr escaping (`< > & "` → entities; `'` NOT escaped;
  `>` IS escaped).
- Entities decoded: named `&lt;&gt;&amp;&quot;&apos;`, numeric `&#65;`/`&#x42;` (hex prefix
  case-insensitive). Unknown entity and bare `&` kept verbatim. CDATA is raw (no decoding inside).
- **HARDENING / recent fix pinned:** a surrogate numeric entity is kept **verbatim**, never crashes —
  `&#xD800;`, `&#55296;`, `&#xDFFF;` all pass through as-is (chr() would throw on them). Also
  out-of-range / malformed numeric bodies (`&#zz;`, `&#xGG;`, `&#1114112;`, `&#;`) kept verbatim.
- `Element(tag, attrib=None)`: default attrib is a fresh `{}`.
- Document shape: no-element text and `""` → `None`; **several top-level siblings → the LAST is root**
  (documented); whitespace-only text preserved.
- **Leniency quirks pinned** (deterministic, worth knowing):
  - `parse("<").tag == ""` — a lone `<` yields an element with an **empty tag string** (NOT `None`).
  - Unquoted attribute values are **not captured**: `<a x=1 y>` → `{"x": "", "1": "", "y": ""}` (only
    quoted values are read; the bare token `1` becomes its own empty attr name).
  - Unterminated comment / CDATA / declaration and angle-bracket soup (`<<<>>>`) never throw.
- **Silent-error probe:** `tostring(non_Element)` correctly **raises** (dispatches `.tostring()` /
  attribute access on the arg) — not a silent no-op. So does `tostring("<a/>")`.

### semver
- `clean` strips leading `v`/`V`/`=`/whitespace. `parse` → `{major,minor,patch,prerelease,build,raw}`
  with pre/build as Lists; throws on invalid. `valid` returns cleaned string or `None` (no throw).
  `major`/`minor`/`patch`/`prerelease` accessors.
- `compare` (−1/0/1): prerelease < release; numeric pre-id < alphanumeric; numeric ids compare
  numerically (`-2` < `-10`); build metadata ignored. `eq/neq/lt/lte/gt/gte` shortcuts; `gt("1.0.10",
  "1.0.9")` True (numeric not lexical). `diff` → major/minor/patch/prerelease/None (build-only → None).
- `inc(s, release)`: supports **only** `major`/`minor`/`patch` (drops prerelease/build); **`inc(s,
  "prerelease")` THROWS** `inc: release must be major/minor/patch`. This matches the docs (which list
  only major/minor/patch) — note the audit task prompt's "inc bumping …/prerelease" is inaccurate vs
  both docs and impl; impl is correct, no change needed.
- Ranges — caret/tilde/x-range/hyphen/comparators/AND(space)/OR(`||`) all exact-tested, incl. the kpm
  table from 04-packages.md (`maxsatisfying` over tags `1.0.0 1.2.0 1.2.5 1.3.0 2.0.0`).
- **CRUCIAL partial-comparator rounding (recent fix) pinned exactly:** `>1`→`>=2.0.0`
  (`satisfies("1.5.0",">1")==False`, `("2.0.0",">1")==True`); `<=1`→`<2.0.0`
  (`("1.9.9","<=1")==True`, `("2.0.0","<=1")==False`); `>1.2`→`>=1.3.0`
  (`("1.2.5",">1.2")==False`, `("1.3.0",">1.2")==True`); `<=1.2`→`<1.3.0`. `>=`/`<` fill with 0.
  Whitespace between operator and version honored (`>= 1.2.0`).
- Prerelease gating (node-semver default): excluded from a plain range; allowed only when a comparator
  in the matched set pins the same major.minor.patch with a prerelease.
- `maxsatisfying`/`minsatisfying` return the **original** version string (v-prefix preserved),
  `None` when nothing matches / empty list. `sort`/`rsort` numeric precedence, invalid dropped.
- `validrange`: `^`/`~`/x-range/comparators/hyphen/`*`/`""` → True; a literal ref like `main`,
  `1.2.3.4` (agrees with `valid`), and garbage → False — this is exactly what kpm relies on.
- `satisfies` never throws: an unparseable range → False.
- **Hardening:** `parse`/`compare`/`major`/`inc` raise clearly on invalid versions/ranges (with the
  documented message text) — no silent wrong answers.

## Assertion counts
- `verify_xml.ki`: ~55 assertions (functioning + entities + tail/itertext + comments/CDATA/decl +
  empty/self-close + Element ctor + escaping + leniency/never-crash + silent-error probes).
- `verify_semver.ki`: ~95 assertions (clean/parse/valid/accessors + compare/precedence + shortcuts +
  diff + inc + full range grammar + partial-comparator rounding + prerelease gating +
  max/minsatisfying + sort/rsort + validrange + hardening/silent-error probes).
