# Analysis NullPointer Unit Tests

add_lotus_test_suite(nullpointer_analysis_tests
  SOURCES
    NullPointerAPITest.cpp
    NullPointerLegacyHelpersTest.cpp
    ContextSensitiveNullCheckAnalysisTest.cpp
  LINK_LIBS
    CanaryNullPointer
    CanaryDyckAA)
