add_lotus_test_suite(alias_lotusaa_tests
  SOURCES
    LotusAATest.cpp
    LotusAAInterproceduralSummaryTest.cpp
    LotusAAConfigurationAndOptimizationTest.cpp
  LINK_LIBS
    CanaryAliasAnalysisWrapper
    LotusAA
    ${Z3_LIBRARIES})

add_lotus_test_suite(alias_gvfg_adapter_tests
  SOURCES
    GVFGWrapperTest.cpp
    GVFGWrapperSummaryInterfaceTest.cpp
  LINK_LIBS
    CanaryGuardedValueFlow
    LotusAA
    ${Z3_LIBRARIES})
