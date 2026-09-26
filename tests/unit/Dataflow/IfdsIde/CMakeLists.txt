# IFDS/IDE Unit Tests

lotus_collect_test_sources(dataflow_tests
  IFDSFrameworkCoreTest.cpp
  IFDSSolverTest.cpp
  TaintAnalysisTest.cpp
  IFDSSolverInvariantsTest.cpp
  PathAwareSolverTest.cpp
  ReturnSiteAndConfigTest.cpp
  IDEEdgeFunctionTest.cpp
  IDEConstantPropagationTest.cpp
  IDEJumpFunctionJoinTest.cpp
  IDENewAnalysesTest.cpp
  LLVMFlowHelpersTest.cpp
  LLVMFlowHelpersMappingTest.cpp
  IFDSClientAliasFlowTest.cpp
  IFDSSolverAliasIntegrationTest.cpp)
