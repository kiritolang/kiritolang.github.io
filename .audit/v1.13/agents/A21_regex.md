# A21 — Regex Engine Audit (v1.13)

Area: `src/kirito/regex_engine.hpp`, `src/kirito/stdlib_regex.hpp`
Auditor: A21 (read-only, static reasoning)
Tests reviewed: `tools/tests/unit/test_regex.cpp`, `test_regex_deep.cpp`, `test_regex_corpus.cpp`, `tools/tests/scripts/probe_regex_conformance.ki`

Status: COMPLETE

## Surface enumerated
- Module `regex` (`stdlib_regex.hpp`): flag consts IGNORECASE/MULTILINE/DOTALL + aliases I/M/S; `compile`, `escape`, one-shot `match/search/fullmatch/findall/finditer/split/sub` (all compile-on-the-fly and delegate to the compiled-Regex method).
- `RegexVal` (compiled Regex): attrs `pattern`/`groups`/`groupindex`; methods `match/search/fullmatch(string,pos,endpos)`, `findall/finditer(string,pos,endpos)`, `sub(repl,string,count)` (string or callable repl), `split(string,maxsplit)`. Not serializable (throws via NativeClass default).
- `MatchVal`: attr `string`; methods `group([keys...])`, `groups(default)`, `groupdict(default)`, `start/end/span(group)`. Group key = Integer index or String name.
- Engine `reng` (`regex_engine.hpp`): recursive-descent Parser -> AST -> Compiler -> flat `Program` bytecode (Char/Any/Class/Match/Jmp/Split/Save/Assert) -> Pike VM `run()` (visited-generation dedup, explicit epsilon-closure stack, per-thread caps vector). Anchors \A \Z/\z ^ $ \b \B; classes incl. \d\w\s + negations, ranges, octal/hex/\u/\U escapes; greedy+lazy `* + ? {n,m} {n,} {n}`; capturing/non-capturing/named `(?P<>)`/`(?<>)`; inline flags `(?ims)`. DELIBERATE rejects: backreferences `\1`/`(?P=)`, lookahead/lookbehind. Guards: repeat count max 1000, numGroups max 1000, program max 200000 insts, parser nesting depth max 2000, hex range/surrogate validation.

---

## CONFIRMED BUGS / WEAK-SPOTS

### A21-1: Large-constant slowdown DoS — linear in input but O(input × program × numGroups) with a ~4×10^8 constant
- severity: MEDIUM
- location: `src/kirito/regex_engine.hpp:592-625` (addThread caps copy), `:641-696` (run main loop), cap rationale at `:522-529`
- category: DoS / weak-spot / performance
- description: The engine's real bound is O(input × program × numGroups), because every `Save`/`Split`/consume transition copies the per-thread `caps` vector of size `capN = 2*(numGroups+1)`. Both factors are capped (numGroups ≤ 1000 => capN ≤ 2002; program ≤ 200000 insts), but the PRODUCT is ~4×10^8 per input position. A pattern with ~1000 nullable capturing groups makes one epsilon-closure copy ~2000 Save vectors × 2002 ints ≈ 4×10^6 int-copies. Repeated per input position this is genuinely slow, though it never OOMs, overflows, or goes exponential — so the "can't blow up" guarantee holds only in the asymptotic-in-input sense, not as a bounded wall-clock.
- failure-scenario: `re.search("()"*1000 + "x", "a"*1_000_000)` — a never-matching pattern with 1000 empty groups over a 1M-char subject. Each of the ~1M positions re-walks the 2000-Save epsilon closure copying a 2002-int caps vector: ~4×10^12 int-copies => multi-minute hang on a single call. Likewise `re.findall("()"*1000, "a"*100000)` (empty-match advance => one run per position). The group cap (1000) is the only mitigation and it is not tight enough given the 200000 program cap.
- proposed-test: a timing test (like the existing `(a+)+b` bound) asserting `re.search("()"*1000+"x", "a"*200000)` completes under a few seconds; if it cannot, tighten the numGroups cap (e.g. 100, as CPython's SRE effectively does) or switch caps to a copy-on-write / arena representation so a Save doesn't deep-copy the whole vector.
- proposed-fix: lower `numGroups` cap substantially (100 is "far beyond any realistic pattern" already) and/or represent caps as a small persistent structure shared across threads (RE2 stores captures out-of-line and only forks on write). Alternatively bound program×numGroups jointly.
- confidence: MEDIUM (mechanism is certain from the code; whether "slow but linear" counts as a bug depends on the threat model, but the audit brief explicitly says to probe that it can't be made to hang).

### A21-2: `match/search/…(string=…, endpos=…)` with `pos` omitted throws (keyword hole filled with None, then asInt(None) fails)
- severity: LOW
- location: `src/kirito/stdlib_regex.hpp:248-267` (match/search/fullmatch), same pattern in finditer/findall; interacts with `native.hpp:187-201` hole-filling
- category: bug / usability
- description: The method params are `{"string","pos","endpos"}` with no defaults; `makeMethod` fills any keyword "hole" before the last supplied arg with `None`. Calling `m.match(string="x", endpos=3)` sets slots 0 and 2, leaving slot 1 (`pos`) a `None` hole, so the impl sees `a.size()==3` and evaluates `args[1].asInt("pos")` on `None` -> throws "expected Integer". You cannot supply `endpos` by keyword without also supplying `pos`.
- failure-scenario: `regex.compile(r"\d").search(string="a1b", endpos=2)` throws instead of searching `a1b[:2]`.
- proposed-test: `regex.compile(r"\d").search(string="a1b2", endpos=2).group() == "1"` (currently throws).
- proposed-fix: give `pos`/`endpos` real defaults (`vm.makeInt(0)` / a sentinel) in the method param spec, or have the impl treat a `None` in the pos/endpos slot as "absent" rather than calling asInt.
- confidence: HIGH (mechanically certain from makeMethod hole-filling + asInt-on-None).

### A21-3: Integer truncation of `pos`/`endpos`/`count`/`maxsplit` (int64 -> int) can silently misbehave on huge values
- severity: LOW
- location: `src/kirito/stdlib_regex.hpp:253,258,275,278,295,298,324,359` (all `static_cast<int>(args[i].asInt(...))`)
- category: bug / -Wconversion-adjacent / overflow
- description: Kirito Integers are int64; every position/count argument is narrowed to `int`. A value like `2**32` truncates to 0 (searches whole string), `2**31` becomes INT_MIN then clamped to 0, etc. No crash (all downstream uses clamp to `[0, size]` or `while(pos<=n)`), but results are silently wrong for out-of-int64-but-in-range inputs. Truncation is implementation-defined for out-of-range signed conversion (C++20 makes it well-defined modulo, so no UB, but the value is surprising).
- failure-scenario: `regex.compile(r"\d").search("a1b", pos=4294967296)` searches from 0 instead of matching nothing.
- proposed-test: `regex.compile(r"\d").search("a1b", 4294967296) == None` (should be no match: pos past end).
- proposed-fix: clamp on the int64 value before narrowing (`std::min<int64_t>(v, size)` then cast), or reject absurd positions.
- confidence: HIGH (visible in code; low impact).

### A21-4: `reng::run()` with `startPos > text.size()` can read out of bounds (engine-level contract, not reachable from Kirito)
- severity: LOW
- location: `src/kirito/regex_engine.hpp:654` (pre-loop addThread at startPos) and `assertHolds:565,573` (`t[sp-1]`)
- category: bug / OOB / robustness
- description: `run()` is a public engine entry point. If called with `startPos > n`, the initial `addThread(..., startPos, ...)` can hit an `Assert` (e.g. `^` multiline or `\b`) whose `assertHolds` reads `t[sp-1]` with `sp = startPos > n` -> out-of-bounds `std::vector::operator[]`. All Kirito call sites clamp `pos <= text.size()` (match/search line 262) or use `allMatches`' `while(pos<=n)` guard, so it is unreachable from scripts — but a C++ embedder calling `reng::run` directly per its documented signature could trip it.
- failure-scenario: `reng::run(compile("^",0), toCodepoints("ab"), 5, false, false)` — startPos 5 > 2, `^` multiline path indexes `t[4]`.
- proposed-test: a C++ unit asserting `run(prog, text, startPos > n, …)` clamps or throws rather than OOB-reads.
- proposed-fix: clamp `startPos` to `[0, n]` at the top of `run()`.
- confidence: MEDIUM (OOB path is real; reachability from public API is nil, hence LOW severity).

### A21-5: Whole pattern is decoded and fully parsed into an AST before the numGroups(≤1000) and program-size caps apply
- severity: LOW
- location: `src/kirito/regex_engine.hpp:515-534` (compile: parse first, `numGroups > 1000` check at :529), parser resizes `names_` per group at `:253`
- category: DoS / weak-spot
- description: `compile()` decodes the entire pattern to code points, parses the full AST, and only THEN rejects `numGroups > 1000`. A pattern like `"(a)"*99000` builds a 99000-group AST (and a 99000-slot `names_`/`groupNames`) before throwing. Memory/time are O(pattern length), so bounded by the caller-provided pattern — not an unbounded blow-up — but there is no early cap during parsing, so a large untrusted pattern does proportional work before rejection.
- failure-scenario: `re.compile("(a)"*200000)` allocates a ~200k-node AST + name vectors before the group-cap throw.
- proposed-test: assert `re.compile("(a)"*100000)` throws promptly and with bounded memory.
- proposed-fix: check `groupCounter_ > 1000` incrementally inside `parseGroup` (throw as soon as the cap is exceeded), and optionally bound raw pattern length up front.
- confidence: MEDIUM.

## COVERAGE GAPS

### A21-6: Absolute anchors `\A`, `\Z`, `\z` are untested
- severity: MEDIUM (coverage)
- location: engine `regex_engine.hpp:315-316` (parseEscape), `assertHolds:563-564`
- category: coverage gap
- description: `^`/`$`/`\b`/`\B` are covered in all three test files, but `\A` (ABeginText) and `\Z`/`\z` (AEndText) have NO test anywhere. Their distinct semantics vs `^`/`$` under MULTILINE, and vs `$`'s before-final-newline behavior, are unverified. Notably `\Z` here means absolute end (sp==n), NOT Python's "end or before trailing newline for `$`" — an easy regression.
- proposed-test: `re.findall(r"(?m)\A\w+", "a\nb")` (only `a`, since `\A` ignores MULTILINE) vs `(?m)^\w+`; `re.fullmatch(r"a\Z","a\n")` == None while `re.search("a$","a\n")` matches.
- confidence: HIGH.

### A21-7: `\u`, `\U`, and hex/octal escape error paths untested
- severity: MEDIUM (coverage)
- location: `regex_engine.hpp:328-349` (readHex, decodeEscapeChar), `:344,347` (range/surrogate throws), `:304` (trailing backslash), `:341` (incomplete \x)
- category: coverage gap
- description: Only `\x41` is tested. Untested: `é`/`\U0001F600` matching; `\U`>0x10FFFF -> throw; surrogate `\uD800` -> throw; incomplete `\x4`/`\u12` -> throw; trailing backslash `"a\\"` -> throw; trailing backslash in class `"[a\\"` -> throw (`:372`).
- proposed-test: `re.fullmatch(r"é","é")` matches; `CHECK_THROWS(compile(r"\uD800"))`; `CHECK_THROWS(compile(r"\U00110000"))`; `CHECK_THROWS(compile("x\\"))`.
- confidence: HIGH.

### A21-8: Structural guards (program-too-large, numGroups cap, nesting-depth cap) untested
- severity: MEDIUM (coverage)
- location: `regex_engine.hpp:428` (200000-inst cap), `:529` (numGroups>1000), `:123` (depth>2000)
- category: coverage gap
- description: `a{5000}` (repeat count) IS tested, but three other DoS guards are not: (a) compiled-program-too-large via nested bounded repeats e.g. `(a{1000}){1000}` -> "compiled pattern too large"; (b) `numGroups>1000` e.g. `"()"*1001` -> "too many capture groups"; (c) parser nesting `"("*1001 + ")"*1001` -> "pattern nested too deeply". These are the core anti-blow-up defenses and none has a test.
- proposed-test: `CHECK_THROWS(compile("("*2001))`; `CHECK_THROWS(compile(std::string("()")*1001))`; `CHECK_THROWS(compile("(a{1000}){1000}"))`.
- confidence: HIGH.

### A21-9: `endpos` as an anchoring boundary (`$`/`\b` at endpos) is untested
- severity: LOW (coverage)
- location: `stdlib_regex.hpp:257-261` (endpos truncates the searched view)
- category: coverage gap
- description: Tests use `endpos` only to bound match length (`match(pos=0,endpos=2)`), never to verify the documented behavior that `$`/`\b` anchor AT endpos (because the view is truncated). E.g. does `re.compile("c$").search("abc",0,3)` match while `...search("abcd",0,3)` also match at index 2?
- proposed-test: `re.compile(r"\bc\b").search("abc def",0,3) != None` and `re.compile("c$").search("abcd",0,3).end() == 3`.
- confidence: MEDIUM.

### A21-10: `\g<0>` (whole-match) template ref and multi-key `group(1,2)` are untested
- severity: LOW (coverage)
- location: `stdlib_regex.hpp:169-176` (groupText allows g==0), `:90-98` (group with multiple keys -> List)
- category: coverage gap
- description: `expandTemplate` explicitly permits group 0 (whole match) via `\g<0>`/`\0`, and `MatchVal::group` returns a List when given several keys — neither path is exercised. Also `groups(default)` with a non-None default on a non-participating group is untested (default forwarding), as is `groupdict(default)`.
- proposed-test: `re.sub("(b)", r"[\g<0>]", "abc") == "a[b]c"`; `re.search("(a)(b)","ab").group(1,2) == ["a","b"]`; `re.search("a(b)?","a").groups("X") == ["X"]`.
- confidence: HIGH.

### A21-11: IGNORECASE folding asymmetry between `charEq` and `classMatches`, and non-Latin folding, untested
- severity: LOW
- location: `regex_engine.hpp:539-557` (classMatches checks lower AND upper of the input; charEq checks lower==lower only)
- category: coverage gap / potential subtle bug
- description: `classMatches` folds the INPUT char both ways (`raw(lower)||raw(upper)`) while `charEq` compares `toLower(a)==toLower(b)`. For code points where `toLower`/`toUpper` are not perfectly invertible within the covered ranges these two dispatch paths could disagree (a literal `[x]` vs a bare `x` under IGNORECASE). Additionally IGNORECASE is only tested on ASCII/Latin-1 (café); Greek/Cyrillic/Turkish-İ folding (unsupported per docs) has no negative test to pin the documented limitation.
- proposed-test: property test asserting for every covered code point c and pattern char p, `search(p, c, I)` matches iff `search("["+p+"]", c, I)` matches; plus a doc-pinning test that `re.search("ς","Σ",re.I)` behaves as specified.
- confidence: LOW (asymmetry may be benign for the covered ranges; worth a property test).

### A21-12: sub/split edge behaviors untested (count > #matches, split maxsplit with captured groups, callable returning None)
- severity: LOW (coverage)
- location: `stdlib_regex.hpp:318-377`
- category: coverage gap
- description: Untested: `sub` count larger than available matches (should replace all); `split` maxsplit combined with a capturing pattern (captured separators + maxsplit interaction, `:370-372`); callable repl returning non-String -> "must return a String" throw; `sub` string repl with a bad `\` reference range e.g. `\9` on a 2-group pattern -> "invalid group reference" throw; template trailing backslash -> throw (`:181`).
- proposed-test: `CHECK_THROWS(re.sub("(a)", r"\9", "a"))`; `re.split("(,)","a,b",1) == ["a", ",", "b"]`; `CHECK_THROWS(re.sub("a", Function(m): return None, "a"))`.
- confidence: HIGH.

### A21-13: `(?<name>)` lookbehind-vs-named disambiguation and alternate rejected group forms partially untested
- severity: LOW (coverage)
- location: `regex_engine.hpp:227-231` (`(?<`), `:220` (`(?=`/`(?!`)
- category: coverage gap
- description: `(?<name>)` named form and `(?<=`/`(?<!` lookbehind rejection are tested, but `(?'name')`, `(?#comment)` (corpus covers `(?#`), and empty inline-flag group repetition `(?i)*` -> "nothing to repeat" are not all covered. The zero-width "nothing to repeat" path for anchors (`^*`, `\b+`) is untested.
- proposed-test: `CHECK_THROWS(compile("^*"))`; `CHECK_THROWS(compile(r"\b?"))`; `CHECK_THROWS(compile("(?i)*"))`.
- confidence: MEDIUM.

## DRY / STRUCTURE NOTES

### A21-14: pos/endpos clamping duplicated across match/finditer/findall; one-shot delegation triplicated
- severity: INFO
- location: `stdlib_regex.hpp:253-262 / 275-281 / 295-301` (identical pos/endpos decode+clamp+resize), and `:458-476` (oneShot / oneShotExtra / the inline sub module fn all repeat compile+getAttr+call)
- category: DRY
- description: The pos/endpos argument decode-clamp-resize block is copy-pasted three times with subtle divergence (match clamps `pos` to resized size at :262; findall/finditer rely on `allMatches`' `while(pos<=n)` instead). The three module one-shot wrappers repeat the same compile→getAttr→call shape. Factor into a shared helper to prevent the paths drifting.
- confidence: HIGH.

## POSITIVE VERIFICATIONS (no bug found)
- Epsilon-loop termination: `visited`/`gen` marks every epsilon pc (incl. Split) so `(a*)*`, `(|a)*`, `()*` cannot infinite-loop; explicit stack in `addThread` avoids native-stack overflow on huge programs. Corpus tests confirm `(a*)*`, `(a|)*` match AND terminate.
- Non-overlapping iteration always progresses: `allMatches` advances `pos = b>a ? b : a+1` and the whole-match start is always `>= startPos`, so `pos` strictly increases — no infinite loop, and empty-match handling matches CPython 3.7+ (`findall(r"\d*","a1b")` == `['', '1', '', '']`).
- Quantifier bounds cannot overflow: `{n,m}` counts clamp to 100000 during accumulation then reject `>1000`; `\U` uses uint32 to avoid signed overflow and rejects `>0x10FFFF` and surrogates.
- Anchor assertions are all bounds-guarded (`sp>0`/`sp<n` before `t[sp-1]`/`t[sp]`); `$` before-final-newline and MULTILINE paths short-circuit before indexing.
- Deliberate rejections all throw cleanly: backreferences `\1`/`(?P=)`, lookahead `(?=`/`(?!`, lookbehind `(?<=`/`(?<!`, `(?#`, unbalanced parens/classes, bad ranges, bad bounds — each with an actionable message; `RegexError` is wrapped as a catchable `KiritoError` ("invalid regex: …") at the module boundary.
- `(a+)+b` on 6000 'a' + 'c' returns None quickly (existing timing test) — no catastrophic backtracking; two independent property fuzzers (3000 + 5000 iters) assert termination + in-bounds/re-fullmatch invariants.
- Match/Regex are correctly non-serializable (NativeClass default throws) — tested.
