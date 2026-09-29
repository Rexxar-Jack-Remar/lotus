# Dataflow Unit Tests

add_subdirectory(IfdsIde)
add_subdirectory(Mono)
add_subdirectory(WPDS)
add_subdirectory(APA)
add_subdirectory(NPA)
add_subdirectory(VASCO)

# All dataflow test sources collected from the subdirectories above are linked
# into one binary so the shared libraries are linked only once.
lotus_collect_test_sources(dataflow_tests DataflowTestMain.cpp)
add_lotus_collected_test_suite(dataflow_tests
  LINK_LIBS
    APADataFlow
    LotusEGraph
    IFDS
    MONODataFlow
    NPADataFlow
    VASCODataFlow
    WPDSDataFlow
    wpds++
    CanaryAliasAnalysisWrapper
    CanaryParallel)

add_test(NAME dataflow_vasco_parallel_harness_workers
  COMMAND $<TARGET_FILE:dataflow_tests> -nworkers=2
    --gtest_filter=VASCOParallelHarnessTest.*)
set_tests_properties(dataflow_vasco_parallel_harness_workers PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  LABELS "lotus;component;dataflow;parallel"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
