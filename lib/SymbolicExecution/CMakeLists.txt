set(CanarySymbolicExecution_SOURCES
  Core/AnalysisLimit.cpp
  Core/BigInteger.cpp
  Core/PropertyValue.cpp
  Core/PropertyInteger.cpp
  Core/PropertySym.cpp
  Core/ProgramVar.cpp
  Core/SymbolicMemory.cpp
  Core/GuardedValue.cpp
  Core/CStringState.cpp
  Core/TaintModel.cpp
  Core/MemoryAPI.cpp
  Core/AnalysisDriver.cpp
  Core/AnalysisState.cpp
  Core/AnalysisSummary.cpp
  Core/AnalysisStateSummary.cpp
  Core/AnalysisStateTaint.cpp
  Solver/ConstraintRepr.cpp
  Solver/PathCondSolver.cpp
  Solver/SummarySolverManager.cpp
  Checks/Query.cpp
  Checks/AnalysisStateQuery.cpp
  Integration/GVFGUtility.cpp
  Integration/SymbolicExecutionWrapper.cpp
)

add_library(CanarySymbolicExecution STATIC ${CanarySymbolicExecution_SOURCES})

target_link_libraries(CanarySymbolicExecution PUBLIC
  CanaryGuardedValueFlow
  CanaryFormats
  #CanarySMT
  LLVMCore
)
