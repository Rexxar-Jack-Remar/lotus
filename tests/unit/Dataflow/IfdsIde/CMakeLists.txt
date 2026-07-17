# IFDS/IDE Unit Tests

add_lotus_test_suite(ifdside_tests
  SOURCES
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
    IFDSSolverAliasIntegrationTest.cpp
  LINK_LIBS
    IFDS
    CanaryAliasAnalysisWrapper)
