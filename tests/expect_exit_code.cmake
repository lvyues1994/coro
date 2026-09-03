if(NOT DEFINED TEST_EXECUTABLE)
    message(FATAL_ERROR "TEST_EXECUTABLE is required")
endif()
if(NOT DEFINED EXPECTED_EXIT_CODE)
    message(FATAL_ERROR "EXPECTED_EXIT_CODE is required")
endif()

execute_process(
    COMMAND "${TEST_EXECUTABLE}"
    RESULT_VARIABLE actual_exit_code)

if(NOT "${actual_exit_code}" STREQUAL "${EXPECTED_EXIT_CODE}")
    message(FATAL_ERROR
        "${TEST_EXECUTABLE} exited with '${actual_exit_code}', expected "
        "'${EXPECTED_EXIT_CODE}' at the designated contract-failure stage")
endif()
