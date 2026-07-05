# Kirito v1.13 comprehensive audit

Deep, full, adversarial audit of the **entire** codebase: correctness/logic bugs, weak spots,
missing input hardening, DRY / single-source-of-truth violations, and — above all — **test coverage**:
every class, method, attribute, argument, function, operator and special method in Kirito's builtins,
standard library, language core, and C++ embedding API must be tested from every angle (typical,
boundary/edge, adversarial, malformed input, fuzz/random, resource-limit), just like the C++ reference.

Then: low-risk high-reward performance work aimed at the **variance** (high stddev) the perf sweep
showed, not raw throughput.

## Durability protocol (read this first)

We will likely hit usage limits. **Nothing may be lost.** Therefore:

- **Every spawned agent owns exactly one file** under `agents/AXX_<area>.md` and writes its findings
  **incrementally** (append as it goes), starting with the file header on the first action — so a
  force-stopped agent still leaves partial findings on disk.
- Audit agents are **READ-ONLY on the codebase**: they modify no source or test file; their only
  write target is their own `.md`.
- The orchestrator commits + pushes `audit_v1_13/` after every wave, so findings survive container
  reclamation, not just a force-stop.

## Phases

1. **Scan** (agents A01–A34): each agent audits its area → durable findings in its `.md`.
2. **Triage**: orchestrator reads all `.md`, dedups + ranks into `FINDINGS.md` (confirmed bugs,
   weak spots, coverage gaps, DRY). Each confirmed bug re-verified before it earns a fix.
3. **Harden + test**: fix confirmed bugs; add C++ tests, then `.ki` tests, closing every coverage
   gap. A fix is only kept if the thing it fixes was genuinely a bug (double-check), the fix is
   idiomatic, and no contract (per `CLAUDE.md`) is broken.
4. **Perf variance**: profile the high-stddev cases; apply only low-risk, measured optimizations.
5. **Docs**: update `CLAUDE.md`, `docs/pages/*`, rebuild the site.
6. **Validate**: full suite across debug → release → asan → tsan.

## Finding format (every agent uses this exactly)

```
### AXX-N: one-line summary
- severity: Critical | High | Medium | Low | coverage-gap | dry
- location: file:line
- category: bug | weak-spot | coverage-gap | dry
- description: ...
- failure scenario: concrete inputs → observed wrong behaviour (for bugs/weak-spots)
- proposed test: adversarial/edge/fuzz case that would catch it
- proposed fix: ...
- confidence: high | medium | low
```

Log confirmed bugs and real weak spots first; log coverage gaps comprehensively. Prefer concrete,
reproducible findings over speculation — but do not omit a gap because it "should" be fine.

## Area assignments

Status: `todo` → `scanning` → `done`. `MD` is the agent's findings file.

| ID | Area | Source file(s) | Existing tests (start here) | Status |
|----|------|----------------|-----------------------------|--------|
| A01 | Lexer | lexer.hpp | test_lexer*, errors/*, scripts probing strings/f-strings/numbers/indent | todo |
| A02 | Parser + AST | parser.hpp, ast.hpp | test_*parse*, errors/*, probe_grammar_fuzz | todo |
| A03 | Resolver + Analyzer + free-vars | resolver.hpp, analyzer.hpp, locals.hpp | test_warnings, test_slot_locals, errors/* | todo |
| A04 | Compiler + bytecode | compiler.hpp, bytecode.hpp | test_slot_locals, bytecode_stability, scripts | todo |
| A05 | Bytecode VM + control/exceptions | bytecode_vm.hpp, control.hpp, exceptions.hpp | test_switch, test_super, scripts exceptions | todo |
| A06 | Runtime: operators/calls/members/numeric | runtime.hpp | test_value_ops, test_class_ops, test_numeric_deep | todo |
| A07 | Object model + class/function values | object.hpp, class_value.hpp, function.hpp | test_class_ops, test_super, test_introspect | todo |
| A08 | Arena + GC + pool + VM + environment | arena.hpp, pool.hpp, vm.hpp, handle.hpp, environment.hpp | test_gc_stress, probe_gc_stress, test_stress | todo |
| A09 | Collections (List/Set/Dict/Array) + hashing | collections.hpp, hashing.hpp | test_list_ops, test_sort, test_collections_deep | todo |
| A10 | Bytes + String methods | bytes.hpp (+ String in runtime) | test_bytes, test_strbytes_deep, test_unicode | todo |
| A11 | Builtins | builtins.hpp | test_builtins_deep, test_namedargs | todo |
| A12 | io + path + streams | stdlib_io.hpp, stdlib_path.hpp | test_io, test_path, test_io_path_deep, test_streams, test_bytesio | todo |
| A13 | math | stdlib_math.hpp | test_numeric_deep, probe_math_vectors, scripts math | todo |
| A14 | random | stdlib_random.hpp | test_random_net_deep, scripts random | todo |
| A15 | tensor + engine | stdlib_tensor.hpp, tensor.hpp | test_tensor, test_tensor_deep, test_multi_index | todo |
| A16 | matrix + complex | stdlib_matrix.hpp, stdlib_complex.hpp | test_complex, scripts matrix/complex | todo |
| A17 | json + serde + serialize + dump | stdlib_json.hpp, stdlib_serde.hpp, stdlib_serialize.hpp, stdlib_dump.hpp | test_json, test_serialize, test_dump, probe_serde_truncation | todo |
| A18 | net + net_compat (freshly expanded) | stdlib_net.hpp, net_compat.hpp | test_net, test_net_primitives, test_net_tls, scripts *net* | todo |
| A19 | sys + time + proc | stdlib_sys.hpp, stdlib_time.hpp, proc_compat.hpp | test_sys, test_time, test_sys_time_deep | todo |
| A20 | zlib + gzip + hash + deflate | deflate.hpp, stdlib_zlib.hpp, stdlib_gzip.hpp, stdlib_hash.hpp | test_zlib, test_hash, test_compress_hash_deep | todo |
| A21 | regex | regex_engine.hpp, stdlib_regex.hpp | test_regex, test_regex_deep, test_regex_corpus, probe_regex_conformance | todo |
| A22 | parallel + dispatcher | stdlib_parallel.hpp, dispatcher.hpp | probe_parallel*, parallel_*, test (tsan) | todo |
| A23 | kimods: itertools/functools/collections/heapq/bisect | stdlib_kimodules.hpp (subset) | test_kimods_deep, test_stdmodules, scripts | todo |
| A24 | kimods: statistics/string/textwrap/copy/enum/tee/arg | stdlib_kimodules.hpp (subset) | test_stdmodules, test_stdlib_extra, scripts | todo |
| A25 | kimods: tabular (Series/DataFrame) | stdlib_kimodules.hpp (subset) | scripts tabular*, audit_tabular | todo |
| A26 | kimods: xml/csv/base64/semver | stdlib_kimodules.hpp (subset), semver via test_semver | test_semver, scripts xml/csv | todo |
| A27 | Classes: inheritance/super/privacy/special methods | runtime.hpp, class_value.hpp | test_class_ops, test_super, spec_bool_dunder | todo |
| A28 | Control flow: if/while/for/switch/with/try/unpack/pack | parser+compiler+vm | test_switch, test_unpacking, scripts | todo |
| A29 | Functions: closures/kwargs/defaults/annotations/inline | multiple | test_namedargs, test_super, scripts | todo |
| A30 | Import system + modules + CLI paths | module.hpp, cli_paths.hpp, main.cpp | probe_import_edges, test_cli_args | todo |
| A31 | C++ embedding API | value.hpp, native.hpp | test_value*, test_pinned_handle, test_cppref_deep, embed_* | todo |
| A32 | kpm + CLI | kpm/kpm.ki, main.cpp | test_kpm, kpm_integration | todo |
| A33 | Coverage cross-reference (meta) | (all) | inspect() surface vs every test | todo |
| A34 | Perf variance (meta) | bench + hot paths | bench, compare.py | todo |
| A35 | DRY / single-source-of-truth (meta) | (all) | — | todo |

## Findings roll-up

`FINDINGS.md` (built in phase 2) is the deduped, ranked master list the fix phase works from.
