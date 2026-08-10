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
install(TARGETS lotus-check
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(LOTUS_BUILD_TESTS)
  set(CHECKER_CLI_TEST_SCRIPT
      ${PROJECT_SOURCE_DIR}/tests/checker_cli/verify_cli.cmake)
  set(CHECKER_CLI_MINIMAL ${PROJECT_SOURCE_DIR}/tests/checker_cli/minimal.ll)
  set(CHECKER_CLI_TAINT ${PROJECT_SOURCE_DIR}/tests/checker_cli/taint.ll)
  set(CHECKER_CLI_TAINT_INVOKE
      ${PROJECT_SOURCE_DIR}/tests/checker_cli/taint_invoke.ll)
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
    math(EXPR arg_count "${index} - 1")
    list(APPEND command -DCLI_ARG_COUNT=${arg_count})
    list(APPEND command -P ${CHECKER_CLI_TEST_SCRIPT})
    add_test(NAME ${test_name} COMMAND ${command})
  endfunction()

  function(add_checker_cli_test_without_output test_name expected_result
           expected_output unexpected_output)
    set(args ${ARGN})
    set(command
        ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:lotus-check>
        -DEXPECTED_RESULT=${expected_result}
        -DEXPECTED_OUTPUT=${expected_output}
        -DUNEXPECTED_OUTPUT=${unexpected_output}
        -DWORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR})
    set(index 1)
    foreach(arg IN LISTS args)
      list(APPEND command -DCLI_ARG${index}=${arg})
      math(EXPR index "${index} + 1")
    endforeach()
    math(EXPR arg_count "${index} - 1")
    list(APPEND command -DCLI_ARG_COUNT=${arg_count})
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
  add_checker_cli_test(checker_cli_taint_fail_on_findings 1
                       "Taint-Style Vulnerability"
                       --engine=taint --sources=source --sinks=sink
                       --fail-on-findings ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_taint_invoke_fail_on_findings 1
                       "Taint-Style Vulnerability"
                       --engine=taint --sources=source --sinks=sink
                       --fail-on-findings ${CHECKER_CLI_TAINT_INVOKE})
  add_checker_cli_test_without_output(
      checker_cli_taint_ignores_operand_bundle 1 "via arg0" "arg1"
      --engine=taint --sources=source --sinks=sink --fail-on-findings
      ${PROJECT_SOURCE_DIR}/tests/checker_cli/taint_operand_bundle.ll)
  add_checker_cli_test(checker_cli_taint_honors_min_score 0
                       "No findings"
                       --engine=taint --sources=source --sinks=sink
                       --report-min-score=81 --fail-on-findings
                       ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_taint_writes_json 0
                       "JSON report written to"
                       --engine=taint --sources=source --sinks=sink
                       --report-json=${CMAKE_CURRENT_BINARY_DIR}/taint-report.json
                       ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_taint_writes_sarif 0
                       "SARIF report written to"
                       --engine=taint --sources=source --sinks=sink
                       --report-sarif=${CMAKE_CURRENT_BINARY_DIR}/taint-report.sarif
                       ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_taint_rejects_missing_suppressions 2
                       "Error loading suppressions"
                       --engine=taint --sources=source --sinks=sink
                       --suppressions=${CMAKE_CURRENT_BINARY_DIR}/missing-suppressions.json
                       ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_forwards_more_than_eight_arguments 0
                       "Analysis completed successfully"
                       --engine=taint --sources=source --sinks=sink
                       --analysis=0 --aa=dyck --show-results=false
                       --max-results=0 --print-stats=false ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_fail_on_findings 1
                       "Memory Leak"
                       --engine=ae --mem-leak --fail-on-findings
                       ${CHECKER_CLI_AE_LEAK})
  add_checker_cli_test(checker_cli_rejects_out_of_range_min_score 2
                       "--report-min-score must be in \\[0,100\\]"
                       --engine=ae --report-min-score=101
                       ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_rejects_engine_after_option_sentinel 2
                       "Too many positional arguments"
                       --engine=taint --sources=source --sinks=sink --
                       --engine=ae ${CHECKER_CLI_TAINT})
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
  add_checker_cli_test(checker_cli_list_checkers_includes_taint 0
                       "IFDS Taint Analysis"
                       --list-checkers)
  add_checker_cli_test(checker_cli_list_checkers_rejects_missing_specs 2
                       "spec directory does not exist"
                       --engine=generic --list-checkers
                       --spec-dir=${CMAKE_CURRENT_BINARY_DIR}/missing-specs)
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
  # Keep the archive delimiters in the library list so they remain adjacent to
  # exactly the static libraries whose registration objects must be retained.
  target_link_libraries(lotus-check PRIVATE
    "-Wl,--whole-archive"
    Saber
    AE
    "-Wl,--no-whole-archive"
  )
endif()
