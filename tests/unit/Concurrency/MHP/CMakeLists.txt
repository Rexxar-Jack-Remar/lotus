lotus_collect_test_sources(concurrency_tests
  MHPBasicAndContextTest.cpp
  MHPThreadFlowGraphTest.cpp
  MHPForkJoinAndSynchronizationTest.cpp
  MHPInterproceduralAndJoinTest.cpp
  StaticVectorClockMHPTest.cpp)

lotus_collect_test_sources(concurrency_tests
  HappensBeforeAnalysisTest.cpp
  HappensBeforeBarrierAndOpenMPTest.cpp
  HappensBeforeAtomicWitnessTest.cpp
  HappensBeforeAtomicMemoryModelTest.cpp
  HappensBeforeOpenMPAndSynchronizationTest.cpp)

lotus_collect_test_sources(concurrency_tests
  AtomicTest.cpp
  AtomicNonConstantTest.cpp
  AtomicFenceAndReleaseSequenceTest.cpp)
