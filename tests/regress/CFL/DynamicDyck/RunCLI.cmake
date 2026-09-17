set(options)
if(DEFINED ALGORITHM)
    list(APPEND options --algorithm ${ALGORITHM})
endif()
execute_process(COMMAND "${BINARY}" ${options} "${MODE}" "${INITIAL}" "${SEQUENCE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "DyckReach failed: ${errors}")
endif()
# The original scripts concatenate timings: one number, one trailing space,
# and no newline or diagnostic output.
if(NOT output MATCHES "^[0-9]+([.][0-9]+)?([eE][+-]?[0-9]+)? $")
    message(FATAL_ERROR "Unexpected legacy timing output: '${output}'")
endif()
