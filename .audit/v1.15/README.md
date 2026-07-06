# v1.15 comprehensive audit — index

Deep audit of the entire codebase: correctness/semantics/logic, memory-safety/UB, silent errors, edge
cases, resource guards, DRY/single-source-of-truth, static analysis, test-coverage completeness (C++ and
.ki), and performance-variance stabilization. Every worker writes findings to its own `scan/<name>.md`
(committed in checkpoints so nothing is lost to usage limits). See `BRIEFING.md`.

## Agent roster & status

### Wave 1 — core engine
- [ ] scan/lexer_parser.md — lexer.hpp, parser.hpp, common.hpp
- [ ] scan/resolver_compiler.md — resolver.hpp, analyzer.hpp, compiler.hpp, locals.hpp, bytecode.hpp
- [ ] scan/vm_runtime.md — bytecode_vm.hpp, runtime.hpp (operators/call/member dispatch)
- [ ] scan/objectmodel_gc.md — object.hpp, arena.hpp, pool.hpp, value.hpp, native.hpp, vm.hpp, module.hpp, function.hpp
- [ ] scan/collections_bytes.md — collections.hpp, bytes.hpp
- [ ] scan/classes_dunders.md — class/instance/super/private-member dispatch
- [ ] scan/numeric.md — numericBinary, Integer/Float/.compare, scalar handling
- [ ] scan/string_format.md — String methods, f-strings, format mini-spec, levenshtein

### Wave 2 — native stdlib
- [ ] scan/math_random.md — stdlib_math.hpp, stdlib_random.hpp
- [ ] scan/tensor.md — tensor.hpp, stdlib_tensor.hpp (engine + autograd)
- [ ] scan/matrix_complex.md — stdlib_matrix.hpp, stdlib_complex.hpp
- [ ] scan/serde.md — stdlib_json/serialize/dump/serde.hpp
- [ ] scan/io_path.md — stdlib_io.hpp, stdlib_path.hpp
- [ ] scan/sys_time_proc.md — stdlib_sys.hpp, stdlib_time.hpp, proc_compat.hpp
- [ ] scan/net.md — stdlib_net.hpp, net_compat.hpp, TLS
- [ ] scan/regex.md — regex_engine.hpp, stdlib_regex.hpp
- [ ] scan/compress_hash.md — deflate.hpp, stdlib_zlib.hpp, stdlib_gzip.hpp, stdlib_hash.hpp

### Wave 3 — ki-modules, builtins, system
- [ ] scan/kimods_data.md — itertools/functools/collections/heapq/bisect/copy/enum
- [ ] scan/kimods_text.md — string/textwrap/base64/csv/tee/arg/semver
- [ ] scan/kimods_tabular_xml.md — tabular, xml
- [ ] scan/builtins.md — all builtins
- [ ] scan/parallel.md — stdlib_parallel.hpp, dispatcher.hpp (concurrency/thread-safety)
- [ ] scan/cli_import.md — main.cpp, cli_paths.hpp, import/module system

### Wave 4 — cross-cutting
- [ ] scan/dry.md — single-source-of-truth / duplicated logic across the codebase
- [ ] scan/coverage_cpp.md — does the C++ unit suite cover every symbol from every angle? gap list
- [ ] scan/coverage_ki.md — does the .ki suite cover every symbol from every angle? gap list
- [ ] scan/static_analysis.md — clang-tidy/cppcheck/max-warnings + perf-variance measurement

## Triage / fix log
(orchestrator fills this after the scan waves — confirmed bugs, fixes, tests, DRY consolidations, perf.)
