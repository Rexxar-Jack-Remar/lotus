# Concurrency Threads Unit Tests
#
# Keep the historical `concurrency_core_tests` target name stable while the
# files now live under finer-grained concurrency subdirectories.

add_lotus_concurrency_test_suite(concurrency_core_tests
  MHPAnalysisTest.cpp
  MHPInterproceduralAndJoinTest.cpp
  StaticVectorClockMHPTest.cpp
  BVClockTest.cpp
  FBVClockTest.cpp
  HappensBeforeAnalysisTest.cpp
  HappensBeforeBarrierAndOpenMPTest.cpp
  HappensBeforeAtomicWitnessTest.cpp
  HappensBeforeAtomicEdgeCasesTest.cpp
  AtomicTest.cpp
  AtomicFenceAndReleaseSequenceTest.cpp
  LockSetAnalysisTest.cpp
  LockSetCoreDataflowTest.cpp
  LockSetDeadlockTest.cpp
  LockSetInterproceduralTest.cpp
  LockSetRAIITest.cpp
  LockSetTryLockTest.cpp
  RAIIMultiLifetimeTest.cpp
  EscapeAnalysisTest.cpp
  StaticThreadSharingAnalysisTest.cpp
  ThreadLocalAnalysisTest.cpp
  ThreadAPITest.cpp
  ThreadAPICppTest.cpp
  ThreadAPICUDATest.cpp
  ThreadAPIKernelTest.cpp
  ThreadAPILockSemanticsTest.cpp
  ThreadAPIOpenMPTest.cpp
  ThreadAPIRuntimeConfigTest.cpp
  JoinTargetAnalysisTest.cpp
  JoinTargetInterproceduralTest.cpp
  ../Kernel/LinuxKernelAnalysisTest.cpp
  ../Kernel/LinuxKernelSynchronizationTest.cpp
  ../Kernel/LinuxKernelExecutionModelTest.cpp
  ../Kernel/LinuxKernelLifecycleTest.cpp
  ../CUDA/CUDAAnalysisTest.cpp
  ../CUDA/CUDAOrderingAndMemoryTest.cpp
  ../CUDA/CUDARuntimeAndUnifiedMemoryTest.cpp
  ../CUDA/CUDAAdvancedSemanticsTest.cpp
  ../CUDA/PTXAnalyzerTest.cpp)
