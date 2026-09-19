lotus_collect_test_sources(concurrency_tests
  MHPAnalysisTest.cpp
  MHPInterproceduralAndJoinTest.cpp
  StaticVectorClockMHPTest.cpp)

lotus_collect_test_sources(concurrency_tests
  HappensBeforeAnalysisTest.cpp
  HappensBeforeBarrierAndOpenMPTest.cpp
  HappensBeforeAtomicWitnessTest.cpp
  HappensBeforeAtomicEdgeCasesTest.cpp)

lotus_collect_test_sources(concurrency_tests
  AtomicTest.cpp
  AtomicNonConstantTest.cpp
  AtomicFenceAndReleaseSequenceTest.cpp)
