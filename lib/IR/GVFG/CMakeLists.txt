add_library(CanaryGuardedValueFlow STATIC
  ConditionRef.cpp
  GuardedValueFlowNodes.cpp
  GuardedValueFlowSites.cpp
  GuardedValueFlowGraph.cpp
  GuardedValueFlowBuilder.cpp
  GuardedValueFlowSolver.cpp
  LotusAdapter.cpp
)

target_link_libraries(CanaryGuardedValueFlow PUBLIC
  LLVMAnalysis
  CanaryGSA
  LotusAA
  CanarySMT
)
