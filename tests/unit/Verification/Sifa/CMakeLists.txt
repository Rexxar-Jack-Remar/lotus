add_lotus_test_suite(verification_sifa_tests
  SOURCES
    SifaReachabilityTest.cpp
    SifaInterproceduralReachabilityTest.cpp
    SifaSymAbsSmokeTest.cpp
    SifaSymAbsLlvmSubsetTest.cpp
    SifaDagInterpreterTest.cpp
    SifaVerificationTest.cpp
  LINK_LIBS
    SifaAnalysis
    SymAbsAIAnalysis
    NPADataFlow
    CanaryAliasAnalysisWrapper
    CanaryParallel
    ${Z3_LIBRARIES}
  TIMEOUT 900)
