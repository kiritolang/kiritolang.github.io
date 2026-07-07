# v1.12.1 comprehensive audit — index

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

Scan status: 27 scanners dispatched; ~10 finished cleanly, the rest were force-stopped by an account
session-limit mid-work but had already written substantial findings to their `.md` (all committed+pushed).
Findings skew LOW (much-audited codebase) with a handful of HIGH/MED. Full per-file detail in `scan/*.md`.

### FIXED (with regression tests in spec_audit_v115.ki, ASan-validated via the asan ki)
- **[HIGH] collections UAF** (scan/collections_bytes.md F2): `DictVal::str`/`SetVal::str` ran a contained
  value's `_str_` (arbitrary user code) with NO `probing_` guard, so a reentrant mutation reallocated the
  live bucket mid-iteration → heap-use-after-free (ASan-confirmed). Fixed: wrap both `str()` bodies in
  `ProbeScope pguard(probing_)`, mirroring `equals()`. Now a clean catchable "changed size" error.
- **[HIGH] tensor.split OOB** (scan/tensor.md F3): a negative section size cast to `size_t` (~SIZE_MAX),
  the sum check overflowed past the axis length, and g_split read out of bounds. Fixed: reject `si < 0`
  before the cast (mirrors the integer-branch `n <= 0` guard).
- **[HIGH] deep-import segfault** (scan/cli_import.md F1): an unbounded deep import chain (a→b→c…)
  recursed the native stack to an uncatchable SIGSEGV. Fixed: cap `importStack_` depth at 500 (wide
  graphs unaffected since it pops on return) → catchable "maximum import nesting depth exceeded".

### CONFIRMED, PENDING (prioritized for the next pass; delicate or lower severity)
- **[HIGH x2 — dunder ownerClass]** (scan/classes_dunders.md F1/F2): `ownerClass` is a single mutable
  field on `KiFunction`; a function object shared as a method across classes (or a module-level function
  adopted as a method) gets the field overwritten → `_super_` climbs the wrong base, and privacy is both
  falsely-denied AND bypassable from external scope (security). Repro CONFIRMED. Fix is delicate (touches
  core method binding — clone-on-adopt vs resolve-owner-from-call-site); needs careful full-suite
  validation. **Deferred to a focused change.**
- [MED] value.hpp comparison operators unchecked `static_cast<BoolVal&>` on a non-Bool dunder result — UB,
  embedding-only (objectmodel_gc.md F1); coverage_cpp.md F1 `Value::str()` on an unbound Value segfaults.
- [MED] lexer trailing-comma inconsistency (lexer_parser.md F1); net.unquote '+'→space + userinfo URL
  reject (net.md F1/F2); io File/BytesIO seek-negative disagreement (io_path.md F1); tabular DataFrame
  index/ragged-row/head-tail validation (kimods_tabular_xml.md F1/F2/F5); semver under-validation
  (kimods_text.md F1); islice negative start (kimods_data.md F2); tensor median NaN + per-axis empty-axis
  identity (tensor.md F1/F4).
- [LOW] NaN prints "-nan" not "nan" (numeric.md F1); complex.polar UB on bad args (matrix_complex.md F1);
  repr DEL, format lone-`}`/comma-zero-pad/empty-precision (string_format.md); DateTime.iso neg-year pad,
  NUL-in-argv truncation (sys_time_proc.md); regex class-range shorthand endpoint (regex.md F1); many more.
- **DRY** (scan/dry.md): F7 the "unhashable type" message HAS ALREADY DRIFTED (Set.remove drops the type
  name) — a real inconsistency; plus String-or-Bytes helper trio, the 256 MiB cap re-declared 7+ places,
  two integer-string parsers, duplicated hex codecs, bound-method-maker copy-pasted ~20×. Consolidate.
- **coverage** (scan/coverage_cpp.md, coverage_ki.md — both partial): gap lists for extending C++ and .ki
  tests to every symbol/angle.
- **perf-variance** (scan/static_analysis.md — thin, agent killed early): needs a re-run to diagnose the
  high-stddev source (likely the adaptive-GC floor) and propose low-risk stabilization.

### FIX ROUND 2 (post-restart)
- **[MED] value.hpp comparison-op UB** (objectmodel_gc.md F1): the 6 `Value::operator==/!=/</<=/>/>=`
  did an unchecked `static_cast<BoolVal&>` on the `applyBinaryOp` result — UB if a user `_eq_`/`_lt_`/…
  returns a non-Bool. Fixed: use `.truthy()` (matches how Kirito's own `if a==b` consumes it).
- **[MED] DRY F7 — "unhashable type" message drift** (dry.md F7): `Set.remove` threw a bare
  "unhashable type" (no type name) while Set.add/Dict/hash() all name the type. Fixed at runtime.hpp:878
  → `unhashable type '<T>'`. Regression test in spec_audit_v115.ki.
- **[REVERTED — was NOT a bug] tensor.median NaN** (tensor.md F4): median sorting NaN-last is a
  deliberate prior decision (sort-defined family), pinned in r7_regressions.ki. The propagate-NaN change
  was reverted. (Re-verify per "double-check the fixed error was actually an error".)
- ASan: spec_audit_v115 (UAF + tensor-split + dunder-clone) + 7 class/collections/tensor tests all clean.

### FIX ROUND 3 (semver / io / repr triage — "double-check it was actually an error")
- **[LOW] repr DEL escape** (string_format.md): `reprString` escaped `< 0x20` but not `0x7f` (DEL), so a
  String containing DEL printed the raw control byte inside a container repr. Fixed → `\x7f`. Regression
  in spec_audit_v115.ki.
- **[MED] semver silent-validation** (kimods_text.md F1): `semver.valid`/`parse` accepted out-of-alphabet
  prerelease/build identifier chars (`1.2.3-bet@`) and an EMPTY build component (`1.2.3+`) — garbage that
  silently validated. Fixed: an `_isident` `[0-9A-Za-z-]` + non-empty check on every prerelease/build id
  (the original "empty prerelease identifier" message is preserved for labx_misc). Regression added.
  - **NOT a bug — leading-zero leniency KEPT**: an initial pass also rejected leading-zero numeric cores
    (`01.2.3`) for strict node-semver conformance, but that leniency is explicitly tested + commented as
    intentional across two rounds (r7/r8_kimods_b: "normalized by Integer()"). Reverted; recorded in the
    false-positives table. Only the genuinely-garbage cases are rejected.
- **[REVERTED — was NOT a bug] BytesIO.seek negative** (io_path.md F1): making `BytesIO.seek(-n)` throw
  "for consistency with File.seek" overturned a DELIBERATE, explicitly-tested clamp-to-0 (r11_stdlib_gaps
  even documents the File-divergence on purpose: "BytesIO seek clamps at 0 (unlike File.seek which
  throws)"). Reverted to the clamp; recorded in the false-positives table.
