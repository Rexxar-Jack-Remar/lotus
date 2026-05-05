# Analysis SymbolicExecution Unit Tests

add_lotus_targeted_test(symbolic_execution_spec_compat_test
               SymbolicExecutionSpecCompatTest.cpp
               LINK_LIBS
                   CanarySymbolicExecution
                   CanaryGuardedValueFlow
                   LLVMAnalysis)
add_lotus_targeted_test(symbolic_execution_taint_model_test
               SymbolicExecutionTaintModelTest.cpp
               LINK_LIBS
                   CanarySymbolicExecution
                   CanaryGuardedValueFlow
                   LLVMAnalysis)
