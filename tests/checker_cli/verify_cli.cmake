if(NOT DEFINED TOOL OR NOT DEFINED EXPECTED_RESULT)
  message(FATAL_ERROR "TOOL and EXPECTED_RESULT are required")
endif()

set(command "${TOOL}")
foreach(index RANGE 1 8)
  if(DEFINED CLI_ARG${index})
    list(APPEND command "${CLI_ARG${index}}")
  endif()
endforeach()

execute_process(
  COMMAND ${command}
  WORKING_DIRECTORY "${WORKING_DIRECTORY}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr)

set(output "${stdout}${stderr}")
if(NOT result EQUAL EXPECTED_RESULT)
  message(FATAL_ERROR
          "unexpected exit code ${result}, expected ${EXPECTED_RESULT}\n${output}")
endif()
if(DEFINED EXPECTED_OUTPUT AND NOT output MATCHES "${EXPECTED_OUTPUT}")
  message(FATAL_ERROR
          "output did not match '${EXPECTED_OUTPUT}'\n${output}")
endif()
if(DEFINED UNEXPECTED_OUTPUT AND output MATCHES "${UNEXPECTED_OUTPUT}")
  message(FATAL_ERROR
          "output unexpectedly matched '${UNEXPECTED_OUTPUT}'\n${output}")
endif()
