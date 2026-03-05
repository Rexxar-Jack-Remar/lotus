set(Concurrency_SOURCES
  Utils/ThreadAPI.cpp
  Utils/ThreadFlowGraph.cpp
  Utils/BVClock.cpp
  Utils/FBVClock.cpp
  Utils/RAIILockTracker.cpp
  Utils/CppAtomics.cpp
  Utils/ThreadLocalAnalysis.cpp
  Utils/OpenMPTaskGraph.cpp
  MHP/MHPAnalysis.cpp
  MHP/StaticVectorClockMHP.cpp
  MHP/HappensBeforeAnalysis.cpp
  LockSet/LockSetAnalysis.cpp
  Memory/EscapeAnalysis.cpp
  Memory/StaticThreadSharingAnalysis.cpp
  Memory/MemUseDefAnalysis.cpp
  JoinTarget/JoinTargetAnalysis.cpp
  MPI/MPIAnalysis.cpp
  MPI/MPIRankAnalysis.cpp
)
add_library(Concurrency STATIC ${Concurrency_SOURCES})
target_link_libraries(Concurrency CanaryLLVMUtils CanaryAliasAnalysisWrapper)
