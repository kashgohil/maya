# Runs COMMAND and passes only when it exits with EXIT_CODE and its output matches EXPECT.
# Usage: cmake "-DCOMMAND=program|argument|argument" -DEXIT_CODE=1 "-DEXPECT=regex" -P expect_exit.cmake
string(REPLACE "|" ";" command "${COMMAND}")
execute_process(COMMAND ${command} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(output "${out}${err}")
if(NOT code STREQUAL "${EXIT_CODE}")
    message(FATAL_ERROR "Exit code ${code}, expected ${EXIT_CODE}. Output:\n${output}")
endif()
if(NOT output MATCHES "${EXPECT}")
    message(FATAL_ERROR "Output does not match '${EXPECT}':\n${output}")
endif()
