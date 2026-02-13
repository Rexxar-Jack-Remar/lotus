set(Concurrency_SOURCES
  Utils/ThreadAPI.cpp
  Utils/ThreadFlowGraph.cpp
  Utils/BVClock.cpp
  Utils/FBVClock.cpp
  Utils/RAIILockTracker.cpp
  MHP/CppAtomics.cpp
  MHP/MHPAnalysis.cpp
  MHP/StaticVectorClockMHP.cpp
  MHP/HappensBeforeAnalysis.cpp
  LockSet/LockSetAnalysis.cpp
  Memory/EscapeAnalysis.cpp
  Memory/StaticThreadSharingAnalysis.cpp
  Memory/MemUseDefAnalysis.cpp
  JoinTarget/JoinTargetAnalysis.cpp
  MPI/MPIAnalysis.cpp
)
add_library(Concurrency STATIC ${Concurrency_SOURCES})
target_include_directories(Concurrency PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(Concurrency CanaryLLVMUtils CanaryAliasAnalysisWrapper)
