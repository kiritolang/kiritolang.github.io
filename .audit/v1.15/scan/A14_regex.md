# A14 Regex

Status: DONE

Scope: src/kirito/regex_engine.hpp, src/kirito/stdlib_regex.hpp (Thompson-NFA Pike VM regex engine).

Read .audit/README.md false-positive table first (done) — nothing regex-specific listed there yet.

Existing test coverage found (good signal of what's already hardened):
- tools/tests/unit/test_regex.cpp, test_regex_corpus.cpp, test_regex_deep.cpp
- tools/tests/scripts/{audit,labx,r4_compress,r6,r7_net,r8_net,spec,spec_corpus,spec_group_cap,spec_oneshot_flags,verify}_regex*.ki

Proceeding to read regex_engine.hpp and stdlib_regex.hpp in full, then cross-reference against test files before filing findings (to avoid false positives already covered).

Read both files in full (regex_engine.hpp 731 lines: parser/AST/compiler/Pike-VM; stdlib_regex.hpp
485 lines: Match/Regex native classes + module). Also enumerated existing coverage: this subsystem
has by far the deepest test suite of anything in the repo — 3 C++ unit files
(test_regex.cpp/test_regex_corpus.cpp/test_regex_deep.cpp) plus 12 golden `.ki` scripts
(audit/labx/probe_regex_conformance/r4_compress/r6/r7_net/r8_net/spec/spec_corpus/spec_group_cap/
spec_oneshot_flags/verify), several of them purpose-built adversarial passes (`labx_regex.ki`,
`r8_net_regex.ki`) that already pin: `a{0}`, `a{,3}` (literal, by design), `a{2,1}` bounds error,
`[z-a]` reversed range, `[]]`/`[^]]` leading-bracket-literal, duplicate named groups (same+cross
spelling), lazy vs greedy for every quantifier shape, `\A`/`\z` staying absolute under MULTILINE,
`$` before-one-trailing-newline semantics, findall/split/sub empty-match advancement, `\g<N>`
overflow rejection, and (a+)+/(a*)* timed catastrophic-pattern checks (test_regex.cpp:151-155,
r8_net_regex.ki:298). Cross-checked every item in the adversarial prompt list against this existing
corpus with Grep before probing further, to avoid re-filing pinned/false-positive behavior.

Ran the binary (`build-release/ki`) directly against the full adversarial list (catastrophic
patterns, `a{2,1}`, `[z-a]`, dup name, `findall("","abc")`, `sub("a*","X","abc")`, `a{999999999}`,
unbalanced paren, trailing backslash, `\1` backreference, `(?=a)`/`(?<=a)` lookaround) — all correct
and consistent with the pinned tests; no new bug there.

Went one level deeper into capture-group semantics for a **nullable body under `*`/`+`**
(`(a*)*`, `(a*)+`, `(a|)*`) against real CPython's `re` as the reference oracle (this module's own
docs model itself on `re`). Found a genuine, currently-untested divergence — see A14-1 below.

---

### A14-1: `(X*)*`/`(X*){0,}`-style repeated-nullable-group captures the wrong iteration (diverges from Python `re`, untested)  [MEDIUM] [HIGH]

- **Location**: `src/kirito/regex_engine.hpp` `detail::compileStar` (lines 473-481) + `addThread`'s
  per-generation `visited` dedup (lines 610-647); root-caused, not a local bug — inherent to how this
  Thompson-NFA/Pike-VM structures the `*` loop-back edge.
- **What**: When a capturing group's body can match the **empty string**, and the whole group is
  itself repeated with `*` (or `{0,}`), Kirito's captured value for that group does NOT match
  Python's `re` (nor Perl/PCRE) in two different ways depending on whether the group also consumed
  real input:
  - **Empty subject**: `re.search("(a*)*", "").groups()` → Kirito `[None]` (group never
    "participated") vs CPython `('',)` (group participated with an empty match). Kirito's `(a*)+`
    (Plus, not Star) on the same input correctly gives `['']` — so the bug is specific to `Star`'s
    compiled shape (Split-first, so the very first Split visit at the same position self-collides
    with the "loop back after a zero-width iteration" visit and the empty-but-participated thread
    is silently dropped by the per-position `visited` generation dedup).
  - **Non-empty subject**: `re.fullmatch("(a*)*", "aaa").groups()` → Kirito `['aaa']` (captures the
    FIRST, all-consuming iteration) vs CPython `('',)` with `span(1) == (3, 3)` (Python's
    backtracking engine runs a second, empty, iteration at the end — matching its documented
    "stop repeating a subpattern once it matches empty" rule (bpo-763) — and that trailing empty
    iteration is what the group ends up holding). Same effect shows up through `sub`:
    `re.sub("(a*)*", "[\\1]", "aaa")` → Kirito `"[aaa][]"` vs Python `"[][]"`.
  - This is the classic, well-documented pitfall of priority-based Thompson-NFA/Pike-VM submatch
    tracking (Russell Cox's "Regular Expression Matching: the Virtual Machine Approach" discusses
    exactly this class of case; RE2 and Go's `regexp` — the engines this module is architecturally
    closest to — are known to diverge from PCRE/Python on repeated-nullable-group captures for the
    same structural reason: the generation-based "visit each pc once per position" bound that
    guarantees linear time is exactly what forces a choice between the "first" and "last" iteration's
    captures, and that choice doesn't always agree with a backtracker). So this is very likely
    **not a quick, isolated bug** but a consequence of the chosen architecture — same category as
    the deliberately-rejected backreferences/lookaround, except this one was not previously
    identified or documented.
- **Repro** (`.ki`, no existing test covers group-index values here — only `.group()`/whole-match is
  checked at `r8_net_regex.ki:298`):
  ```
  var re = import("regex")
  var io = import("io")
  io.print(re.search("(a*)*", "").groups())          # Kirito: [None]   Python: ('',)
  io.print(re.fullmatch("(a*)*", "aaa").groups())     # Kirito: ['aaa']  Python: ('',)
  io.print(re.fullmatch("(a*)*", "aaa").span(1))      # Kirito: [0, 3]   Python: (3, 3)
  io.print(re.sub("(a*)*", "[\\1]", "aaa"))           # Kirito: "[aaa][]"  Python: "[][]"
  ```
- **Proposed fix**: Given the architecture, a full PCRE-exact fix (tracking "last executed iteration"
  captures through a linear-time simulation) is a nontrivial engine change, not a one-line patch —
  recommend one of: (a) accept the divergence explicitly, documenting it in `CLAUDE.md`'s regex
  bullet alongside the backreference/lookaround rejections (e.g. "capture values for a repeated
  group whose body can match empty may not match Python's `re` — a documented Thompson-NFA
  tradeoff, same class as RE2/Go `regexp`"), or (b) if worth the engineering cost, special-case
  `Star`/`Repeat` bodies that are statically nullable to compile a guarded "enter once unconditionally,
  then only loop while making progress" shape (closer to `Plus`'s already-correct behavior) so at
  least the *empty-subject* case (`(a*)*` on `""` giving `None` instead of `''`) is fixed — that half
  looks more tractable than the "last vs first iteration on a non-empty match" half.
- **Proposed test**: pin whichever behavior is decided as correct-going-forward with the four exact
  assertions above (currently zero test files check `.groups()`/`.span(N)` — as opposed to
  `.group()`, the whole match — for `(a*)*`/`(a*)+` patterns).

---

## Summary

This subsystem (`regex_engine.hpp` + `stdlib_regex.hpp`) is the most heavily pre-hardened corner of
the codebase encountered so far: 3 C++ unit test files plus 12 golden `.ki` scripts already
adversarially cover every item on the standard "break a regex engine" checklist — catastrophic
patterns (`(a+)+b`, `(a*)*`) verified linear via wall-clock timing, `a{2,1}`/`[z-a]`/dup-name/
huge-repetition/unbalanced-paren/trailing-backslash/backreference/lookahead/lookbehind all throw
clean, well-worded `RegexError`s, `findall`/`sub`/`split` all correctly advance past empty matches,
`\g<N>` overflow is rejected, `pos`/`endpos`/keyword-hole-fill semantics are exercised, and both
group-count-across-alternation-branches and named-group access are verified.

One genuine, previously-unflagged, and currently-untested finding survived that scrutiny:

- **A14-1** (MEDIUM/HIGH confidence): capture values for a **repeated group whose body can match the
  empty string** (`(a*)*`/`(a*){0,}`) diverge from Python's `re` in both the empty-subject case
  (group shows as non-participating instead of `''`) and the non-empty-subject case (captures the
  first/greedy iteration instead of the trailing empty one Python's backtracker leaves behind, which
  also shows up through `sub`). This is architecturally the same class of tradeoff as RE2/Go's
  `regexp` vs PCRE — likely not a quick fix — but it was undocumented and untested (existing tests
  only check `.group()`, never `.groups()`/`.span(N)`, for this pattern shape), so at minimum it
  needs a documented, pinned decision one way or the other.

No memory-safety, crash, hang, or malformed-pattern-handling defects were found; the compile-time
guards (nesting depth 2000, instruction count 200000, group count 1000, repetition count 1000, the
`kMaxMatchWork` capture-volume budget) and the linear-time Pike-VM core all held up under adversarial
probing. Coverage is otherwise complete enough that I would not recommend spending further scanner
budget re-probing this subsystem's already-tested surface (quantifier edge cases, class edge cases,
anchors/boundaries, flags, escapes, malformed-pattern rejection) — it is exhaustively pinned.

