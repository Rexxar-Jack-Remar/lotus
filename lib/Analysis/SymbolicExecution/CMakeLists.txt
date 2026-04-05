set(CanarySymbolicExecution_SOURCES
  AnalysisLimit.cpp
  SegUtility.cpp
  BigInteger.cpp
  PropertyValue.cpp
  PropertyInteger.cpp
  PropertySym.cpp
  ProgramVar.cpp
  ConstraintRepr.cpp
  PathCondSolver.cpp
  TaintModel.cpp
  MemoryAPI.cpp
  AnalysisDriver.cpp
  AnalysisState.cpp
  AnalysisStateQuery.cpp
  AnalysisStateSummary.cpp
  AnalysisStateTaint.cpp
  AnalysisStateUtils.cpp
  SymbolicExecutionWrapper.cpp
)

add_library(CanarySymbolicExecution STATIC ${CanarySymbolicExecution_SOURCES})

target_link_libraries(CanarySymbolicExecution PUBLIC
  CanaryGuardedValueFlow
  #CanarySMT
  #LLVMCore
  LLVMAnalysis
)
