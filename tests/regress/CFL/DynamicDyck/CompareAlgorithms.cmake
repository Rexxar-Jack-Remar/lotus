execute_process(COMMAND "${BINARY}" --stats --print-components "${INITIAL}" "${SEQUENCE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE original ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Original solver failed: ${errors}")
endif()
execute_process(COMMAND "${BINARY}" --algorithm primary-component --backend "${BACKEND}"
    --stats --print-components "${INITIAL}" "${SEQUENCE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE added ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "POPL 2024 solver failed: ${errors}")
endif()
foreach(output original added)
    string(REGEX MATCH "vertices=[^\n]*changed_updates=[0-9]+" ${output}_stats "${${output}}")
    string(REGEX MATCHALL "component:[^\n]*" ${output}_components "${${output}}")
endforeach()
if(NOT original_stats STREQUAL added_stats OR
   NOT original_components STREQUAL added_components)
    message(FATAL_ERROR "Algorithms differ:\nOriginal:\n${original}\nPOPL 2024:\n${added}")
endif()
if(NOT added MATCHES "algorithm=primary-component backend=${BACKEND} semantics=set")
    message(FATAL_ERROR "Missing POPL 2024 selection diagnostic: ${added}")
endif()
