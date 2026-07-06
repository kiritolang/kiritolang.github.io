# regex — doc-vs-impl verification findings

Verified `docs/pages/10-stdlib.md` (`## regex`) + `docs/pages/26-bonus-01-regex.md` against
`src/kirito/stdlib_regex.hpp` + `src/kirito/regex_engine.hpp` with
`tools/tests/scripts/verify_regex.ki` (145 assertions, expected `OK regex`).

## Discrepancies

**None.** Every documented function, method, attribute, flag, syntax construct, and error message
matches the implementation. No silent bugs found; every documented rejection actually throws, and
with the message the docs/impl promise.

## Behaviour PINNED (exact current behaviour, in case it ever drifts)

### Return shapes
- `findall` shape by group count: 0 groups → List of matched Strings; 1 group → List of that
  group's Strings; 2+ groups → List of per-match Lists. (`re.findall(r"(\w)(\d)", "a1b2")` →
  `[['a','1'],['b','2']]`.)
- `finditer` (module + Regex) returns an eagerly-built **List** of `Match` objects (not a lazy
  iterator), non-overlapping, leftmost. `re.finditer("aa","aaaa")` → 2 matches (non-overlapping).
- `split` interleaves captured groups; splits on empty matches too, so an empty-capable pattern
  yields leading/inter-character `''`s: `re.split("", "abc")` → `['', 'a', 'b', 'c', '']`.
- Empty/zero-width: `re.findall("", "abc")` → `['','','','']`; `re.findall("a*","aba")` →
  `['a','','a','']`; `re.search("a*","")` matches `""`.

### Match object
- `group()`/`group(0)` = whole match; `group(n)`/`group(name)`; several keys → List; a
  non-participating group → `None`. Absent group: `start`/`end` = `-1`, `span` = `[-1,-1]`.
- `groups([default])`, `groupdict([default])` (named groups only), `start/end/span([key])`,
  `.string` attribute. `str(match)` = `<Match span=(a, b)>`.

### Regex object
- Attributes `.pattern`, `.groups` (Integer count), `.groupindex` (Dict name→number).
  `str(regex)` = `<Regex /PATTERN/>`.
- `pos`/`endpos` are code-point indices; `endpos` truncates the subject so `$`/`\b` anchor there.
- **Hole-fill fix confirmed working**: `p.search("aaa", endpos=2)` with `pos` omitted (passed as a
  later keyword) works — the skipped leading optional `pos=None` is treated as "use default 0", not a
  type error.

### Flags & syntax
- `IGNORECASE=1`, `MULTILINE=2`, `DOTALL=4`; aliases `I`/`M`/`S` identical. Combine with `+`.
- Inline flags `(?i)`/`(?m)`/`(?s)` apply to the **whole** pattern regardless of position
  (`re.search("a(?i)b","AB")` matches) — deliberately unlike Python.
- Shorthands `\d \w \s` (and negations) are **ASCII-only**: `\w+` on `"naïve"` → `["na","ve"]`
  (the non-ASCII `ï` is not a word char). Note the deliberate asymmetry with `escape`, which treats
  any code point > 127 as "wordish" and leaves it unescaped (`escape("héllo")` == `"héllo"`) — both
  behaviours are intentional and documented, not a bug.
- `\A` = absolute start (ignores MULTILINE), `\z`/`\Z` = absolute end, `\b`/`\B` word boundary.
- Octal is `\0NN` (leading-zero, up to 2 further octal digits ⇒ max `\077`); a bare `\1`–`\9` is
  rejected as a backreference. `\b` inside a `[...]` class is a backspace (0x08).
- Escapes `\xHH \uHHHH \UHHHHHHHH`; a value > U+10FFFF or a UTF-16 surrogate is rejected.

### Rejections (all throw `invalid regex: ...` at compile) — messages pinned
- Backreferences: `\1`, `(a)\1` → `backreferences are not supported ...`; `(?P=name)` →
  `named backreferences are not supported`.
- Lookaround: `(?=)`/`(?!)` → `lookahead ... is not supported ...`; `(?<=)`/`(?<!)` →
  `lookbehind ... is not supported ...`.
- Syntax: `missing ) in pattern`, `unbalanced ')' in pattern`, `nothing to repeat`,
  `trailing backslash in pattern`, `unterminated character class`, `bad character range in class`,
  `bad repetition bounds {3,1}`, `duplicate group name 'x'`, `empty group name`, `bad group name '1x'`,
  `incomplete \x/\u escape`, `escape value out of range (above U+10FFFF)`,
  `escape is a UTF-16 surrogate (not a scalar code point)`.
- Structural caps: `repetition count too large (max 1000)` (`a{2000}`),
  `too many capture groups (max 1000)` (`()`×1001), `pattern nested too deeply` (2001 nested `(`),
  `compiled pattern too large` (`(?:a{1000}){1000}`).

### Match-object / sub misuse (runtime KiritoError, catchable)
- `no such group: 9`, `no such group: 'nope'`,
  `group key must be an Integer index or a String name`.
- `sub` template invalid group ref → `invalid group reference N in replacement template`;
  unknown `\g<name>` → `bad replacement: no group named '...'`.
- `sub` callable returning non-String → `sub replacement function must return a String`;
  `sub` repl neither String nor callable → `sub replacement must be a String or a function`.

### Linear-time guarantee
- `re.search("(a+)+b", "a"*30 + "c")` returns `None` and completes in well under 1s (asserted
  < 1000 ms via `time.perfcounterns`) — no catastrophic backtracking.
