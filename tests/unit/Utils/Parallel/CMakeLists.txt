lotus_collect_test_sources(utils_tests
  PipelineSchedulerTest.cpp
  PipelineSchedulerTimeoutTest.cpp
  ParallelSchedulerPassTest.cpp
  ThreadSafeTest.cpp
  ThreadPoolHarnessTest.cpp
  ParallelTabulationHarnessTest.cpp
  PipelineSchedulerHarnessTest.cpp)

# The discovered GTest cases use the default zero-worker pool. Repeat the
# worker-dependent harness cases with two real workers.
string(CONCAT utils_parallel_harness_filter
  "ThreadPoolHarnessTest.*:ParallelTabulationHarnessTest.*:"
  "PipelineSchedulerHarnessTest.*")
add_test(NAME utils_parallel_harness_workers
  COMMAND $<TARGET_FILE:utils_tests> -nworkers=2
    --gtest_filter=${utils_parallel_harness_filter})
set_tests_properties(utils_parallel_harness_workers PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  LABELS "lotus;component;utils;parallel"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
