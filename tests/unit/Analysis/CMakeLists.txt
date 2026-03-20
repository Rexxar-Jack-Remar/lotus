# Analysis Unit Tests

add_lotus_test(debug_info_analysis_test DebugInfoAnalysisTest.cpp)
add_lotus_test(nullpointer_api_test NullPointerAPITest.cpp)
add_lotus_test(nullpointer_legacy_helpers_test NullPointerLegacyHelpersTest.cpp)
add_lotus_test(context_sensitive_nullcheck_analysis_test
               ContextSensitiveNullCheckAnalysisTest.cpp)
add_lotus_test(spectre_analysis_test SpectreAnalysisTest.cpp)
add_lotus_test(crypto_analysis_test CryptoAnalysisTest.cpp)
