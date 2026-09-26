# Concurrency MPI Unit Tests

lotus_collect_test_sources(concurrency_tests
  MPIAnalysisTest.cpp
  MPIAnalysisTest_Collectives.cpp
  MPIAnalysisTest_RMA.cpp
  MPIAnalysisTest_Communicators.cpp
  MPIAnalysisTest_State.cpp)

lotus_collect_test_sources(concurrency_tests
  MPIRankAnalysisTest_Basic.cpp
  MPIRankAnalysisTest_Range.cpp
  MPIRankAnalysisTest_Predicate.cpp)
