file(MAKE_DIRECTORY "${WORK}")
configure_file("${FIXTURE}" "${WORK}/input.dot" COPYONLY)

execute_process(
    COMMAND python3 "${SCRIPT}" input.dot --tool "${PROGRAM}"
    WORKING_DIRECTORY "${WORK}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT "${status}" STREQUAL "0")
    message(FATAL_ERROR "graph reduction failed: ${status}\n${output}\n${error}")
endif()
if(NOT output MATCHES "finish one iteration")
    message(FATAL_ERROR "graph reduction did not run both modes\n${output}")
endif()
