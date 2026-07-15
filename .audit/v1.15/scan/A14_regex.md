# A14 Regex

Status: IN PROGRESS

Scope: src/kirito/regex_engine.hpp, src/kirito/stdlib_regex.hpp (Thompson-NFA Pike VM regex engine).

Read .audit/README.md false-positive table first (done) — nothing regex-specific listed there yet.

Existing test coverage found (good signal of what's already hardened):
- tools/tests/unit/test_regex.cpp, test_regex_corpus.cpp, test_regex_deep.cpp
- tools/tests/scripts/{audit,labx,r4_compress,r6,r7_net,r8_net,spec,spec_corpus,spec_group_cap,spec_oneshot_flags,verify}_regex*.ki

Proceeding to read regex_engine.hpp and stdlib_regex.hpp in full, then cross-reference against test files before filing findings (to avoid false positives already covered).
