# Concurrency OpenMP Unit Tests

add_lotus_concurrency_test_suite(concurrency_openmp_tests
  OpenMPTaskGraphTest.cpp
  OpenMPTaskGraphTestPart2.cpp
  OpenMPSemanticsTest.cpp
  DataSharingAnalysisTest.cpp
  ConcurrencyFacadeTest.cpp)
