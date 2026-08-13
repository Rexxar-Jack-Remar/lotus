# Concurrency Threads Unit Tests
#
# Keep the historical `concurrency_core_tests` target name stable while the
# files now live under finer-grained concurrency subdirectories.

add_lotus_concurrency_test_suite(concurrency_core_tests
  MHPAnalysisTest.cpp
  MHPAnalysisTestPart2.cpp
  StaticVectorClockMHPTest.cpp
  BVClockTest.cpp
  FBVClockTest.cpp
  HappensBeforeAnalysisTest.cpp
  HappensBeforeAnalysisTestPart2.cpp
  HappensBeforeAnalysisTestPart3.cpp
  HappensBeforeAnalysisTestPart4.cpp
  AtomicTest.cpp
  AtomicTestPart2.cpp
  LockSetAnalysisTest.cpp
  LockSetAnalysisTestPart2.cpp
  RAIIMultiLifetimeTest.cpp
  EscapeAnalysisTest.cpp
  StaticThreadSharingAnalysisTest.cpp
  ThreadLocalAnalysisTest.cpp
  ThreadAPITest.cpp
  ThreadAPITestPart2.cpp
  JoinTargetAnalysisTest.cpp
  JoinTargetAnalysisTestPart2.cpp
  ../Kernel/LinuxKernelAnalysisTest.cpp
  ../CUDA/CUDAAnalysisTest.cpp
  ../CUDA/CUDAAnalysisTestPart2.cpp
  ../CUDA/CUDAAnalysisTestPart3.cpp
  ../CUDA/PTXAnalyzerTest.cpp)
