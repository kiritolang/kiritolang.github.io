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
if(DEFINED INPUT AND NOT INPUT STREQUAL "")
    execute_process(COMMAND ${KI} ${SCRIPT} ${_script_args} INPUT_FILE ${INPUT}
                    OUTPUT_VARIABLE actual RESULT_VARIABLE rc)
else()
    execute_process(COMMAND ${KI} ${SCRIPT} ${_script_args}
                    OUTPUT_VARIABLE actual RESULT_VARIABLE rc)
endif()

file(READ ${EXPECTED} expected)
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
        "Script ${SCRIPT} output mismatch (exit ${rc}).\n"
        "--- expected ---\n${expected}\n--- actual ---\n${actual}")
endif()
