# Analysis DebugInfo Unit Tests

add_lotus_test_suite(debug_info_analysis_tests
  SOURCES
    DebugInfoAnalysisTest.cpp
    UniqueIRMarkerTest.cpp
  LINK_LIBS CanaryDebugInfo)
