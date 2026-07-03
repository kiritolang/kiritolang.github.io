# One small executable per test file (one behavior-area each), linking the header-only core.
find_package(Threads REQUIRED)

# Shared precompiled header. Every test TU does `#include "kirito.hpp"`, which pulls in the entire
# header-only interpreter — parsing it once per TU dominates the build. This carrier compiles that
# umbrella into a PCH ONCE; each test reuses it (REUSE_FROM below), so a clean build parses the core
# a single time instead of ~60 times. The carrier carries the exact compile setup the tests use
# (same warnings, same kirito/Threads usage requirements) so the PCH is binary-compatible to reuse.
add_library(kirito_pch STATIC pch_carrier.cpp)
target_link_libraries(kirito_pch PRIVATE kirito Threads::Threads)
target_compile_options(kirito_pch PRIVATE -Wall -Wextra -Wpedantic)
target_precompile_headers(kirito_pch PRIVATE ${CMAKE_SOURCE_DIR}/src/kirito.hpp)

function(kirito_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE kirito Threads::Threads)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    target_precompile_headers(${name} REUSE_FROM kirito_pch)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

kirito_test(test_arena unit/test_arena.cpp)
kirito_test(test_lexer unit/test_lexer.cpp)
kirito_test(test_cli_paths unit/test_cli_paths.cpp)
kirito_test(test_eval_arith unit/test_eval_arith.cpp)
kirito_test(test_eval_vars unit/test_eval_vars.cpp)
kirito_test(test_indent unit/test_indent.cpp)
kirito_test(test_control unit/test_control.cpp)
kirito_test(test_conditional unit/test_conditional.cpp)
kirito_test(test_functions unit/test_functions.cpp)
kirito_test(test_bytecode unit/test_bytecode.cpp)
kirito_test(test_resolver unit/test_resolver.cpp)
kirito_test(test_collections unit/test_collections.cpp)
kirito_test(test_modules unit/test_modules.cpp)
kirito_test(test_strings unit/test_strings.cpp)
kirito_test(test_string_literals unit/test_string_literals.cpp)
kirito_test(test_gc unit/test_gc.cpp)
kirito_test(test_exceptions unit/test_exceptions.cpp)
kirito_test(test_exceptions_cpp unit/test_exceptions_cpp.cpp)
kirito_test(test_exceptions_deep unit/test_exceptions_deep.cpp)
kirito_test(test_classes unit/test_classes.cpp)
kirito_test(test_kwargs unit/test_kwargs.cpp)
kirito_test(test_tabular unit/test_tabular.cpp)
kirito_test(test_xml unit/test_xml.cpp)
kirito_test(test_audit_fixes unit/test_audit_fixes.cpp)
kirito_test(test_audit_findings unit/test_audit_findings.cpp)
kirito_test(test_audit_round2 unit/test_audit_round2.cpp)
kirito_test(test_audit_regressions unit/test_audit_regressions.cpp)
kirito_test(test_domain_guards unit/test_domain_guards.cpp)
kirito_test(test_vm_serialization unit/test_vm_serialization.cpp)
# CLI argument-handling test: drives the built ki as a subprocess (popen). The actual ki binary
# isn't a link-time dependency, but the test needs it on disk — depend on the ki target and pass
# its location as argv[1] so the test is hermetic.
add_executable(test_cli_args unit/test_cli_args.cpp)
target_compile_features(test_cli_args PRIVATE cxx_std_20)
target_link_libraries(test_cli_args PRIVATE kirito)
target_precompile_headers(test_cli_args REUSE_FROM kirito_pch)
add_dependencies(test_cli_args ki)
add_test(NAME test_cli_args COMMAND test_cli_args $<TARGET_FILE:ki>)
kirito_test(test_protocol unit/test_protocol.cpp)
kirito_test(test_value_extra unit/test_value_extra.cpp)
kirito_test(test_embedding_extra unit/test_embedding_extra.cpp)
kirito_test(test_r4_cpp_api unit/test_r4_cpp_api.cpp)
kirito_test(test_r5_internals unit/test_r5_internals.cpp)
kirito_test(test_r6_internals unit/test_r6_internals.cpp)
kirito_test(test_r7_embed_api unit/test_r7_embed_api.cpp)
kirito_test(test_r8_embed_api unit/test_r8_embed_api.cpp)
kirito_test(test_proc unit/test_proc.cpp)
kirito_test(test_serde unit/test_serde.cpp)
kirito_test(test_serialize_native unit/test_serialize_native.cpp)
kirito_test(test_with unit/test_with.cpp)
kirito_test(test_string_ops unit/test_string_ops.cpp)
kirito_test(test_stdlib unit/test_stdlib.cpp)
kirito_test(test_extras unit/test_extras.cpp)
kirito_test(test_fstring unit/test_fstring.cpp)
kirito_test(test_references unit/test_references.cpp)
kirito_test(test_imports unit/test_imports.cpp)
kirito_test(test_builtins unit/test_builtins.cpp)
kirito_test(test_hasattr unit/test_hasattr.cpp)
kirito_test(test_random unit/test_random.cpp)
kirito_test(test_matrix unit/test_matrix.cpp)
kirito_test(test_complex unit/test_complex.cpp)
kirito_test(test_tensor unit/test_tensor.cpp)
kirito_test(test_multi_index unit/test_multi_index.cpp)
kirito_test(test_class_ops unit/test_class_ops.cpp)
kirito_test(test_json unit/test_json.cpp)
kirito_test(test_net unit/test_net.cpp)
kirito_test(test_serialize unit/test_serialize.cpp)
kirito_test(test_sys unit/test_sys.cpp)
kirito_test(test_time unit/test_time.cpp)
kirito_test(test_dump unit/test_dump.cpp)
kirito_test(test_zlib unit/test_zlib.cpp)
kirito_test(test_bytes unit/test_bytes.cpp)
kirito_test(test_apply unit/test_apply.cpp)
kirito_test(test_hash unit/test_hash.cpp)
kirito_test(test_regex unit/test_regex.cpp)
kirito_test(test_regex_corpus unit/test_regex_corpus.cpp)
kirito_test(test_sort unit/test_sort.cpp)
kirito_test(test_hostile_libs unit/test_hostile_libs.cpp)
kirito_test(test_io unit/test_io.cpp)
kirito_test(test_path unit/test_path.cpp)
kirito_test(test_namedargs unit/test_namedargs.cpp)
kirito_test(test_adversarial unit/test_adversarial.cpp)
kirito_test(test_unpacking unit/test_unpacking.cpp)
kirito_test(test_stdmodules unit/test_stdmodules.cpp)
kirito_test(test_semver unit/test_semver.cpp)
kirito_test(test_kpm unit/test_kpm.cpp)
# kpm.ki lives at the repo root, not under tests/; hand its directory to the test via the environment
# so `import("kpm")` resolves it (passed by env, not a -D, to keep the shared PCH reusable).
set_tests_properties(test_kpm PROPERTIES ENVIRONMENT "KPM_DIR=${CMAKE_SOURCE_DIR}/kpm")
# kpm end-to-end integration: drives the REAL kpm under `ki` against a localhost mock git host
# (GitHub- and GitLab-shaped), exercising install/deps/semver/update/remove/version-conflict and every
# misconfiguration failure mode. Needs python3 + the built `ki`; skipped if python3 is absent.
find_program(KPM_PYTHON NAMES python3 python)
if(KPM_PYTHON)
    add_test(NAME kpm_integration
             COMMAND ${KPM_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/kpm_integration.py $<TARGET_FILE:ki>)
    set_tests_properties(kpm_integration PROPERTIES TIMEOUT 120)
endif()
kirito_test(test_introspect unit/test_introspect.cpp)
kirito_test(test_warnings unit/test_warnings.cpp)
kirito_test(test_unicode unit/test_unicode.cpp)
kirito_test(test_hardening unit/test_hardening.cpp)
kirito_test(test_hardening2 unit/test_hardening2.cpp)
kirito_test(test_stdlib_extra unit/test_stdlib_extra.cpp)
kirito_test(test_super unit/test_super.cpp)
kirito_test(test_stdlib_gaps unit/test_stdlib_gaps.cpp)
kirito_test(test_list_ops unit/test_list_ops.cpp)
kirito_test(test_bytesio unit/test_bytesio.cpp)
kirito_test(test_switch unit/test_switch.cpp)
kirito_test(test_slot_locals unit/test_slot_locals.cpp)
kirito_test(test_streams unit/test_streams.cpp)
kirito_test(test_stress unit/test_stress.cpp)
kirito_test(test_value unit/test_value.cpp)
kirito_test(test_modules_fuzz unit/test_modules_fuzz.cpp)

# Multiprocessing (KiritoDispatcher + the `parallel` module). Every blocking scenario runs under an
# in-test deadlock watchdog; the CTest TIMEOUT is a coarse backstop so a regression that slips past
# the watchdog still fails the suite rather than stalling CI. RUN_SERIAL: each test spawns its own pool
# of worker threads, so `ctest -j` must NOT run them alongside other tests — core oversubscription on a
# few-core box (e.g. WSL) starves the workers and trips the watchdog/timeout. Running them alone (full
# cores each) keeps them fast and reliable everywhere.
kirito_test(test_parallel unit/test_parallel.cpp)
kirito_test(test_parallel_sync unit/test_parallel_sync.cpp)
kirito_test(test_parallel_deadlock unit/test_parallel_deadlock.cpp)
kirito_test(test_parallel_random unit/test_parallel_random.cpp)
kirito_test(test_parallel_edge unit/test_parallel_edge.cpp)
kirito_test(test_parallel_fuzz unit/test_parallel_fuzz.cpp)
set_tests_properties(test_parallel test_parallel_sync test_parallel_deadlock test_parallel_random
                     test_parallel_edge test_parallel_fuzz PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

kirito_test(embed_demo integration/embed_demo.cpp)
# --- embed_* integration projects: real C++ programs that embed KiritoVM ---
kirito_test(embed_strat_sim integration/embed_strat_sim.cpp)
kirito_test(embed_dag_engine integration/embed_dag_engine.cpp)
kirito_test(embed_rules integration/embed_rules.cpp)
kirito_test(embed_calc integration/embed_calc.cpp)
kirito_test(embed_router integration/embed_router.cpp)
kirito_test(embed_lint integration/embed_lint.cpp)
kirito_test(embed_pipe integration/embed_pipe.cpp)
kirito_test(embed_sql integration/embed_sql.cpp)
kirito_test(embed_dsl integration/embed_dsl.cpp)
kirito_test(embed_ga integration/embed_ga.cpp)
kirito_test(fuzz_eval fuzz/fuzz_eval.cpp)
kirito_test(bench bench/bench.cpp)

# --- end-to-end .ki script tests (run through the `ki` interpreter, diff vs .expected) ---
# A .ki file is only registered as a test if it has a matching .expected; this lets library
# files (imported by other scripts, e.g. complex.ki) live alongside without being run directly.
# CONFIGURE_DEPENDS so a newly-added scripts/*.ki is picked up by a plain `cmake --build` (which
# re-globs), not only after a manual reconfigure — otherwise a new test silently isn't registered.
file(GLOB ki_scripts CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.ki)
foreach(script ${ki_scripts})
    get_filename_component(nm ${script} NAME_WE)
    set(expected ${CMAKE_CURRENT_SOURCE_DIR}/scripts/${nm}.expected)
    if(NOT EXISTS ${expected})
        continue()
    endif()
    set(input ${CMAKE_CURRENT_SOURCE_DIR}/scripts/${nm}.in)
    set(extra "")
    if(EXISTS ${input})
        set(extra -DINPUT=${input})
    endif()
    # Optional `<name>.args` sidecar (one argv token per line) supplies the script's command-line
    # arguments, so a test of arglist/argmain runs with identical argv in every runner (CTest,
    # test_release.sh, the self-host). Pass the FILE PATH (a single value) — not a pre-joined list —
    # and let run_ki_script.cmake re-read it with file(STRINGS). A joined ';'-list would expand
    # INLINE in add_test, scattering the tokens as separate `cmake` arguments so only the first
    # reached `-DARGS` (and a literal `--` token would even abort cmake's own option parsing before
    # `-P`, silently skipping the run) — so multi-token args never actually reached the script.
    set(argfile ${CMAKE_CURRENT_SOURCE_DIR}/scripts/${nm}.args)
    if(EXISTS ${argfile})
        list(APPEND extra -DARGSFILE=${argfile})
    endif()
    add_test(NAME script_${nm}
             COMMAND ${CMAKE_COMMAND} -DKI=$<TARGET_FILE:ki> -DSCRIPT=${script}
                     -DEXPECTED=${expected} ${extra}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ki_script.cmake)
endforeach()

# The parallel golden scripts can block on a real bug; cap them so a regression fails rather than
# stalling CI (the watchdog backstop, mirroring the parallel unit tests' TIMEOUT).
foreach(nm parallel_basic parallel_sync probe_parallel probe_parallel_adversarial)
    if(TEST script_${nm})
        set_tests_properties(script_${nm} PROPERTIES TIMEOUT 60)
    endif()
endforeach()

# --- error-message tests: .ki files that MUST fail, with a .experr of required diagnostic text ---
# Verifies the interpreter reports actionable errors (syntax, indentation, runtime, annotations)
# rather than crashing or going silent. Auto-discovered, so new cases need no CMake edits.
file(GLOB ki_error_scripts ${CMAKE_CURRENT_SOURCE_DIR}/errors/*.ki)
foreach(script ${ki_error_scripts})
    get_filename_component(nm ${script} NAME_WE)
    set(expected ${CMAKE_CURRENT_SOURCE_DIR}/errors/${nm}.experr)
    if(NOT EXISTS ${expected})
        continue()
    endif()
    add_test(NAME error_${nm}
             COMMAND ${CMAKE_COMMAND} -DKI=$<TARGET_FILE:ki> -DSCRIPT=${script}
                     -DEXPECTED=${expected}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ki_error.cmake)
endforeach()

# --- the `imaging` example package (examples/big_projects/imaging): run its self-test through ki ---
# A pure-Kirito Pillow-style image library built on the tensor backend. Its modules resolve via the
# script's own directory (on the import path), so no copies are needed here. The self-test is an
# assert-driven .ki whose final lines are matched against test_imaging.expected.
set(_imaging_dir ${CMAKE_SOURCE_DIR}/examples/big_projects/imaging)
add_test(NAME script_imaging
         COMMAND ${CMAKE_COMMAND} -DKI=$<TARGET_FILE:ki>
                 -DSCRIPT=${_imaging_dir}/test_imaging.ki
                 -DEXPECTED=${_imaging_dir}/test_imaging.expected
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ki_script.cmake)

# The video module (jpeg/gif codecs + an OpenCV-style VideoCapture) over the committed testdata/
# assets. Run with the package dir as the working directory so the `testdata/...` paths resolve.
add_test(NAME script_video
         COMMAND ${CMAKE_COMMAND} -DKI=$<TARGET_FILE:ki>
                 -DSCRIPT=${_imaging_dir}/test_video.ki
                 -DEXPECTED=${_imaging_dir}/test_video.expected
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ki_script.cmake)
set_tests_properties(script_video PROPERTIES WORKING_DIRECTORY ${_imaging_dir})

# --- the `kirdown` example package: a Markdown -> HTML converter in pure Kirito ---
# Exercises the regex, arg, path, io, hash, and time stdlib modules. Self-test is an assert-driven
# .ki whose final line is matched against test_kirdown.expected. Modules live alongside the script
# so the interpreter's default import path resolves them.
set(_kirdown_dir ${CMAKE_SOURCE_DIR}/examples/big_projects/kirdown)
add_test(NAME script_kirdown
         COMMAND ${CMAKE_COMMAND} -DKI=$<TARGET_FILE:ki>
                 -DSCRIPT=${_kirdown_dir}/test_kirdown.ki
                 -DEXPECTED=${_kirdown_dir}/test_kirdown.expected
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ki_script.cmake)

# --- the `ledger` example package: plain-text double-entry accounting in pure Kirito ---
# Exercises regex, arg, enum, heapq, statistics, csv, hash, path, io, and time. The self-test
# creates its own temp fixture files under path.gettempdir(); modules live alongside the script.
set(_ledger_dir ${CMAKE_SOURCE_DIR}/examples/big_projects/ledger)
add_test(NAME script_ledger
         COMMAND ${CMAKE_COMMAND} -DKI=$<TARGET_FILE:ki>
                 -DSCRIPT=${_ledger_dir}/test_ledger.ki
                 -DEXPECTED=${_ledger_dir}/test_ledger.expected
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ki_script.cmake)

# --- the `cronki` example package: cron-like scheduler + one-shot batch runner ---
# Exercises regex, time (DateTime arithmetic + strptime), arg, sys.shell (real subprocess),
# hash, json, path, io. The self-test spawns `echo`/`false` via sys.shell so it needs a working
# /bin/sh; results go to a tempdir file, not the working tree.
set(_cronki_dir ${CMAKE_SOURCE_DIR}/examples/big_projects/cronki)
add_test(NAME script_cronki
         COMMAND ${CMAKE_COMMAND} -DKI=$<TARGET_FILE:ki>
                 -DSCRIPT=${_cronki_dir}/test_cronki.ki
                 -DEXPECTED=${_cronki_dir}/test_cronki.expected
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ki_script.cmake)

# --- the `feedreader` example package: RSS 2.0 + Atom feed reader in pure Kirito ---
# Exercises xml (parses both feed families), regex (stripHtml + search), hash (stable item keys),
# json (state on disk), textwrap (wrapped `show`), net (only in `sync`; the self-test uses local
# sample xml files and does not go through the network).
set(_feedreader_dir ${CMAKE_SOURCE_DIR}/examples/big_projects/feedreader)
add_test(NAME script_feedreader
         COMMAND ${CMAKE_COMMAND} -DKI=$<TARGET_FILE:ki>
                 -DSCRIPT=${_feedreader_dir}/test_feedreader.ki
                 -DEXPECTED=${_feedreader_dir}/test_feedreader.expected
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ki_script.cmake)
set_tests_properties(script_feedreader PROPERTIES WORKING_DIRECTORY ${_feedreader_dir})

# --- the `snip` example package: a code snippet manager in pure Kirito ---
# Exercises dump (primary on-disk format, reference-preserving), json (portable export),
# hash (sha256 for stable ids), regex (search filter), arg (subcommand CLI), time (humanAge).
set(_snip_dir ${CMAKE_SOURCE_DIR}/examples/big_projects/snip)
add_test(NAME script_snip
         COMMAND ${CMAKE_COMMAND} -DKI=$<TARGET_FILE:ki>
                 -DSCRIPT=${_snip_dir}/test_snip.ki
                 -DEXPECTED=${_snip_dir}/test_snip.expected
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ki_script.cmake)

# --- the self-host interpreter is a STANDALONE big project, NOT wired into CTest ---
# examples/big_projects/selfhost is a Kirito interpreter written in Kirito; its run_tests.ki re-runs
# the .ki suite THROUGH it (a tree-walker on a tree-walker). It's the language's most demanding
# stress test but also the slowest by far (~15-40 min) and tracks the CORE language, not the native
# stdlib — so it lives apart from the main suite and is run by hand now and then, not on every build:
#
#   ./build-release/ki --lib examples/big_projects/selfhost/lib \
#                      examples/big_projects/selfhost/run_tests.ki        # (pass `full` for the slow set)
#
# Run from the repo root. It exits non-zero if any core-language program diverges.
