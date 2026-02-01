set(Concurrency_SOURCES
  Cpp11Atomics.cpp
  EscapeAnalysis.cpp
  HappensBeforeAnalysis.cpp
  JoinTargetAnalysis.cpp
  LockSetAnalysis.cpp
  MemUseDefAnalysis.cpp
  MHPAnalysis.cpp
  StaticThreadSharingAnalysis.cpp
  StaticVectorClockMHP.cpp
  ThreadAPI.cpp
  ThreadFlowGraph.cpp
)
add_library(Concurrency STATIC ${Concurrency_SOURCES})
target_link_libraries(Concurrency CanaryLLVMUtils CanaryAliasAnalysisWrapper)

