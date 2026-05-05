# Analysis General Unit Tests

add_lotus_spectre_test(spectre_analysis_test SpectreAnalysisTest.cpp)
add_lotus_constant_time_test(crypto_analysis_test CryptoAnalysisTest.cpp)
add_lotus_targeted_test(prue_pass_test
               PruePassTest.cpp
               LINK_LIBS
                   CanaryNisse
                   LLVMAnalysis
                   LLVMTransformUtils)
