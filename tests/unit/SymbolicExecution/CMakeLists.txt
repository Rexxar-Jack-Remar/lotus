# SymbolicExecution Unit Tests

add_lotus_test_suite(symbolic_execution_tests
  SOURCES
    SymbolicExecutionSpecCompatTest.cpp
    SymbolicExecutionTaintModelTest.cpp
  LINK_LIBS
    CanarySymbolicExecution
    CanaryGuardedValueFlow
    LLVMAnalysis)
