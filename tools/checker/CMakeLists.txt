# Unified checker front-end.

include(GNUInstallDirs)

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Analysis
  IRReader
  Passes
)

add_executable(lotus-check
  CheckerReport.cpp
  lotus-check-generic.cpp
  lotus-check-kint.cpp
  lotus-check-taint.cpp
  lotus-check-concur.cpp
  lotus-check-pulse.cpp
  lotus-check-fitx.cpp
  lotus-check-saber.cpp
  lotus-check-ae.cpp
  lotus-check-symex.cpp
)

target_link_libraries(lotus-check PRIVATE
  Annotation
  CanaryAliasAnalysisWrapper
  CanaryFuzzing
  CanaryKint
  CanaryReport
  CanarySymbolicExecution
  CheckerCore
  ConcurrencyChecker
  FiTxChecker
  IFDS
  PulseChecker
  ${LLVM_LINK_COMPONENTS}
  ${Z3_LIBRARIES}
)

target_compile_definitions(lotus-check PRIVATE
  LOTUS_INSTALL_CHECKER_SPEC_DIR="${CMAKE_INSTALL_FULL_DATADIR}/lotus/checkers"
  LOTUS_SOURCE_CHECKER_SPEC_DIR="${PROJECT_SOURCE_DIR}/config/checkers"
)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/config/checkers/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/lotus/checkers)

if(LOTUS_BUILD_TESTS)
  set(CHECKER_CLI_TEST_SCRIPT
      ${PROJECT_SOURCE_DIR}/tests/checker_cli/verify_cli.cmake)
  set(CHECKER_CLI_MINIMAL ${PROJECT_SOURCE_DIR}/tests/checker_cli/minimal.ll)
  set(CHECKER_CLI_TAINT ${PROJECT_SOURCE_DIR}/tests/checker_cli/taint.ll)
  set(CHECKER_CLI_AE_LEAK ${PROJECT_SOURCE_DIR}/tests/checker_cli/ae_leak.ll)

  function(add_checker_cli_test test_name expected_result expected_output)
    set(args ${ARGN})
    set(command
        ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:lotus-check>
        -DEXPECTED_RESULT=${expected_result}
        -DEXPECTED_OUTPUT=${expected_output}
        -DWORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR})
    set(index 1)
    foreach(arg IN LISTS args)
      list(APPEND command -DCLI_ARG${index}=${arg})
      math(EXPR index "${index} + 1")
    endforeach()
    list(APPEND command -P ${CHECKER_CLI_TEST_SCRIPT})
    add_test(NAME ${test_name} COMMAND ${command})
  endfunction()

  add_checker_cli_test(checker_cli_ae_defaults_include_mem_leak 0
                       "Running memory leak checker"
                       --engine=ae ${CHECKER_CLI_MINIMAL})
  foreach(detector IN ITEMS df leak uaf)
    add_checker_cli_test(checker_cli_fitx_${detector}_selection 0
                         "Detector: ${detector}"
                         --engine=fitx --detector=${detector}
                         ${CHECKER_CLI_MINIMAL})
  endforeach()
  add_checker_cli_test(checker_cli_fitx_rejects_unknown_detector 2
                       "unknown FiTx detector"
                       --engine=fitx --detector=foobar ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_concur_rejects_substring_check 2
                       "unknown concurrency check: raceXYZ"
                       --engine=concur --checks=raceXYZ ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_taint_micro_bench_evaluates 0
                       "TP: 1  FP: 0  FN: 0"
                       --engine=taint --micro-bench
                       --expected=${PROJECT_SOURCE_DIR}/tests/checker_cli/taint.expected
                       ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_taint_trims_function_lists 0
                       "\\[source\\] source"
                       --engine=taint --verbose "--sources=source, source"
                       --sinks=sink ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_fail_on_findings 1
                       "Memory Leak"
                       --engine=ae --mem-leak --fail-on-findings
                       ${CHECKER_CLI_AE_LEAK})
  add_checker_cli_test(checker_cli_report_io_error 2
                       "Error writing JSON report"
                       --engine=fitx --detector=uaf
                       --report-json=${CMAKE_CURRENT_BINARY_DIR}/missing/report.json
                       ${CHECKER_CLI_MINIMAL})
  add_test(
    NAME checker_cli_generic_finds_specs_outside_source_tree
    COMMAND ${CMAKE_COMMAND}
            -DTOOL=$<TARGET_FILE:lotus-check>
            -DEXPECTED_RESULT=0
            -DEXPECTED_OUTPUT=taint.getenv-system
            -DWORKING_DIRECTORY=/tmp
            -DCLI_ARG1=--engine=generic
            -DCLI_ARG2=--list-checkers
            -P ${CHECKER_CLI_TEST_SCRIPT})
endif()

# Force linkage of all symbols from Saber and AE libraries so their checker
# registration and option definitions are retained in the merged binary.
if(APPLE)
  target_link_options(lotus-check PRIVATE
    -Wl,-force_load,$<TARGET_FILE:Saber>
    -Wl,-force_load,$<TARGET_FILE:AE>
  )
  target_link_libraries(lotus-check PRIVATE Saber AE)
else()
  target_link_options(lotus-check PRIVATE -Wl,--whole-archive)
  target_link_libraries(lotus-check PRIVATE Saber AE)
  target_link_options(lotus-check PRIVATE -Wl,--no-whole-archive)
endif()
