# Fails when BINARY (an executable or static library) contains Dear ImGui symbols.
# Usage: cmake -DBINARY=<path> -P check_no_editor_ui.cmake
if(NOT BINARY)
    message(FATAL_ERROR "BINARY is required")
endif()
execute_process(COMMAND nm "${BINARY}" OUTPUT_VARIABLE symbols ERROR_VARIABLE errors RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "nm failed for ${BINARY}: ${errors}")
endif()
string(REGEX MATCH "[^\n]*ImGui[^\n]*" found "${symbols}")
if(found)
    message(FATAL_ERROR "${BINARY} depends on the editor UI framework: ${found}")
endif()
message(STATUS "${BINARY} has no editor UI symbols")
