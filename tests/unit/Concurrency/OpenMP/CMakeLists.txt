# Concurrency OpenMP Unit Tests

lotus_collect_test_sources(concurrency_tests
  OpenMPTaskGraphTest.cpp
  OpenMPTaskGraphAdvancedSemanticsTest.cpp
  OpenMPSemanticsEventsTest.cpp
  OpenMPSemanticsRegionsTest.cpp
  OpenMPSemanticsTasksTest.cpp
  OpenMPSemanticsRuntimeValidationTest.cpp
  DataSharingAnalysisTest.cpp
  ConcurrencyFacadeTest.cpp)
