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
