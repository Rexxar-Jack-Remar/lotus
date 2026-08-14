# Concurrency OpenMP Unit Tests

add_lotus_concurrency_test_suite(concurrency_openmp_tests
  OpenMPTaskGraphTest.cpp
  OpenMPTaskGraphAdvancedSemanticsTest.cpp
  OpenMPSemanticsEventsTest.cpp
  OpenMPSemanticsRegionsTest.cpp
  OpenMPSemanticsTasksTest.cpp
  OpenMPSemanticsRuntimeValidationTest.cpp
  DataSharingAnalysisTest.cpp
  ConcurrencyFacadeTest.cpp)
