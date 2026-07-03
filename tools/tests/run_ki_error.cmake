# Run a .ki script that is EXPECTED TO FAIL, and assert on the diagnostic it emits.
# The script must exit non-zero, and its stderr must contain every line listed in the matching
# .experr file (each line is a required substring — kept loose so exact line:col numbers can shift
# without breaking the test, while the human-actionable message text is pinned).
# Args (via -D): KI (interpreter), SCRIPT, EXPECTED (the .experr file).
execute_process(COMMAND ${KI} ${SCRIPT}
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)

if(rc EQUAL 0)
    message(FATAL_ERROR
        "Script ${SCRIPT} was expected to FAIL but exited 0.\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()

file(STRINGS ${EXPECTED} needles)
foreach(needle ${needles})
    if(needle STREQUAL "")
        continue()
    endif()
    string(FIND "${err}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR
            "Script ${SCRIPT} (exit ${rc}) diagnostic missing expected text.\n"
            "--- wanted substring ---\n${needle}\n--- actual stderr ---\n${err}")
    endif()
endforeach()
