# Run a .ki script through `ki` and compare stdout to a golden .expected file.
# Args (via -D): KI (interpreter), SCRIPT, EXPECTED, optional INPUT (stdin file),
# optional ARGSFILE (a `<name>.args` sidecar, one argv token per line). The sidecar is passed by
# PATH and read here with file(STRINGS) so the token list never touches this invocation's own
# command line — each line becomes exactly one element of _script_args, so a token may contain
# spaces or be option-shaped (e.g. `--`) and still reach the script intact. (file(STRINGS) skips
# blank lines, so an empty-string argv token cannot be expressed via the sidecar.)
if(DEFINED ARGSFILE AND NOT ARGSFILE STREQUAL "")
    file(STRINGS ${ARGSFILE} _script_args)
else()
    set(_script_args)
endif()
# Optional KIFLAGS: interpreter flags placed BEFORE the script path (e.g. `--no-inline`). Lets the same
# golden be run under a semantics-preserving switch and diffed against the SAME .expected, proving the
# switch changes speed, not results. Space-separated -> a list.
if(DEFINED KIFLAGS AND NOT KIFLAGS STREQUAL "")
    separate_arguments(_ki_flags UNIX_COMMAND "${KIFLAGS}")
else()
    set(_ki_flags)
endif()
if(DEFINED INPUT AND NOT INPUT STREQUAL "")
    execute_process(COMMAND ${KI} ${_ki_flags} ${SCRIPT} ${_script_args} INPUT_FILE ${INPUT}
                    OUTPUT_VARIABLE actual RESULT_VARIABLE rc)
else()
    execute_process(COMMAND ${KI} ${_ki_flags} ${SCRIPT} ${_script_args}
                    OUTPUT_VARIABLE actual RESULT_VARIABLE rc)
endif()

file(READ ${EXPECTED} expected)
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
        "Script ${SCRIPT} output mismatch (exit ${rc}).\n"
        "--- expected ---\n${expected}\n--- actual ---\n${actual}")
endif()
