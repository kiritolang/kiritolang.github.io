# Regex engine audit (v1.12.1)

Subsystem: `src/kirito/regex_engine.hpp` (parser → bytecode → Pike VM), `src/kirito/stdlib_regex.hpp` (Kirito API).

## LOG
- Read both source files fully.
- CONFIRMED linear-time: (a+)+b, (a*)*b, (a|a|a)*b, (a?){30}a{30} all sub-ms on 40-char adversarial input. No catastrophic backtracking.
- CONFIRMED correct: findall 0/1/2 groups, non-participating groups (''), group() None, groups(default), named (?P<>/(?<>), duplicate-name reject.
- CONFIRMED correct: sub \1/\g<n>/\g<name>/\0/callable/negative-count(=all)/empty-pat/bad-group/trailing-bs; split/maxsplit/captures/empty-pat.
- CONFIRMED all rejections: backref, lookahead/behind (+neg), named backref, unbalanced ()/[], dangling quant, bad range, bad rep bounds, huge rep (>1000), trailing bs, empty/bad group name.
- CONFIRMED flags: IGNORECASE ascii+latin1+class, MULTILINE ^/$, DOTALL, inline (?i)/(?m); perl greedy-alt (a|ab -> "a"); greedy/lazy; \A \b \B; $ before final newline.
- CONFIRMED pos/endpos (incl huge/neg clamp, endpos affects $), start/end/span/group-by-name, bad-group throws.
- RULED OUT (by-design leniency, not bugs): {,3} treated as literal '{' (not {0,3}); [\d-z] treats '-' as literal; [-a]/[a-]/[]a] literal handling.

## FINDINGS
### F1 [LOW] Character-class range with a shorthand-escape high endpoint silently becomes a nonsense literal range
- where: src/kirito/regex_engine.hpp:385-391 (parseClass range branch), via classSingleEscape->decodeEscapeChar default
- repro: `re.findall(r"[a-\d]", "abcd5-x")` => `['a','b','c','d']`
- actual: `[a-\d]` is parsed as the range a..'d' (the `\d` shorthand degrades to the literal char 'd'); the digit-class intent is silently lost.
- expected: Python raises "bad character range"; at minimum the `\d` should contribute the digit class, not a literal 'd'. Same applies to `[a-\w]`, `[\x41-\s]`, etc. — any shorthand as the high end of a range.
- fix idea: in parseClass, when the range's hi side is an escape, detect a shorthand (d/w/s/D/W/S) and reject the range (or treat '-' as literal) instead of falling through classSingleEscape which returns the letter itself.
