# Analysis Unit Tests

add_lotus_debug_info_test(debug_info_analysis_test DebugInfoAnalysisTest.cpp)
add_lotus_analysis_test(nullpointer_api_test NullPointerAPITest.cpp)
add_lotus_analysis_test(nullpointer_legacy_helpers_test NullPointerLegacyHelpersTest.cpp)
add_lotus_analysis_test(context_sensitive_nullcheck_analysis_test
               ContextSensitiveNullCheckAnalysisTest.cpp)
add_lotus_spectre_test(spectre_analysis_test SpectreAnalysisTest.cpp)
add_lotus_crypto_test(crypto_analysis_test CryptoAnalysisTest.cpp)
add_lotus_targeted_test(loop_analysis_foundation_test
               LoopAnalysisFoundationTest.cpp
               LINK_LIBS
                   CanaryLoopAnalysis
                   CanaryCFG
                   CanaryDebugInfo
                   CanaryAliasAnalysisWrapper
                   LLVMAnalysis)
add_lotus_targeted_test(loop_dependence_graph_test
               LoopDependenceGraphTest.cpp
               LINK_LIBS
                   CanaryLoopAnalysis
                   CanaryCFG
                   CanaryDebugInfo
                   CanaryPDG
                   CanaryAliasAnalysisWrapper
                   LLVMAnalysis)
add_lotus_targeted_test(loop_scalar_analysis_test
               LoopScalarAnalysisTest.cpp
               LINK_LIBS
                   CanaryLoopAnalysis
                   CanaryCFG
                   CanaryDebugInfo
                   CanaryPDG
                   CanaryAliasAnalysisWrapper
                   LLVMAnalysis)
add_lotus_targeted_test(loop_environment_and_carried_deps_test
               LoopEnvironmentAndCarriedDepsTest.cpp
               LINK_LIBS
                   CanaryLoopAnalysis
                   CanaryCFG
                   CanaryDebugInfo
                   CanaryPDG
                   CanaryAliasAnalysisWrapper
                   LLVMAnalysis)
add_lotus_targeted_test(loop_iteration_space_test
               LoopIterationSpaceTest.cpp
               LINK_LIBS
                   CanaryLoopAnalysis
                   CanaryCFG
                   CanaryDebugInfo
                   CanaryPDG
                   CanaryAliasAnalysisWrapper
                   LLVMAnalysis)
add_lotus_targeted_test(loop_classification_test
               LoopClassificationTest.cpp
               LINK_LIBS
                   CanaryLoopAnalysis
                   CanaryCFG
                   CanaryDebugInfo
                   CanaryPDG
                   CanaryAliasAnalysisWrapper
                   LLVMAnalysis)
