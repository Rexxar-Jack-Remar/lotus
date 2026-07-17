# Analysis General Unit Tests

add_lotus_test_suite(analysis_general_tests
  SOURCES
    SpectreAnalysisTest.cpp
    CryptoAnalysisTest.cpp
    PruePassTest.cpp
  LINK_LIBS
    Spectre
    ConstantTimeVerify
    CanaryNisse
    LLVMAnalysis
    LLVMTransformUtils)
