# Analysis Loop Unit Tests

add_lotus_targeted_test(loop_analysis_foundation_test
               LoopAnalysisFoundationTest.cpp
               LINK_LIBS
                   CanaryLoopAnalysis
                   CanaryCFG
                   CanaryDebugInfo
                   CanaryPDG
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
add_lotus_targeted_test(loop_ldg_builder_test
               LoopLDGBuilderTest.cpp
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
add_lotus_targeted_test(loop_nesting_graph_test
               LoopNestingGraphTest.cpp
               LINK_LIBS
                   CanaryLoopAnalysis
                   CanaryCFG
                   CanaryDebugInfo
                   CanaryPDG
                   CanaryAliasAnalysisWrapper
                   LLVMAnalysis)
add_lotus_targeted_test(loop_parity_iv_test
               LoopParityIVTest.cpp
               LINK_LIBS
                   CanaryLoopAnalysis
                   CanaryCFG
                   CanaryDebugInfo
                   CanaryPDG
                   CanaryAliasAnalysisWrapper
                   LLVMAnalysis)
add_dependencies(loop_parity_iv_test noelle_loop_parity_ll_fixtures)
target_compile_definitions(loop_parity_iv_test PRIVATE
  LOTUS_NOELLE_LOOP_PARITY_FIXTURE_DIR="${NOELLE_LOOP_PARITY_SRC_DIR}"
  LOTUS_NOELLE_LOOP_PARITY_LL_DIR="${NOELLE_LOOP_PARITY_LL_DIR}")
add_test(NAME LoopParityIVTest COMMAND ${LOTUS_TEST_BIN_DIR}/loop_parity_iv_test)
set_tests_properties(LoopParityIVTest PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_lotus_targeted_test(loop_parity_iteration_space_test
               LoopParityIterationSpaceTest.cpp
               LINK_LIBS
                   CanaryLoopAnalysis
                   CanaryCFG
                   CanaryDebugInfo
                   CanaryPDG
                   CanaryAliasAnalysisWrapper
                   LLVMAnalysis)
add_dependencies(loop_parity_iteration_space_test noelle_loop_parity_ll_fixtures)
target_compile_definitions(loop_parity_iteration_space_test PRIVATE
  LOTUS_NOELLE_LOOP_PARITY_FIXTURE_DIR="${NOELLE_LOOP_PARITY_SRC_DIR}"
  LOTUS_NOELLE_LOOP_PARITY_LL_DIR="${NOELLE_LOOP_PARITY_LL_DIR}")
add_test(NAME LoopParityIterationSpaceTest COMMAND ${LOTUS_TEST_BIN_DIR}/loop_parity_iteration_space_test)
set_tests_properties(LoopParityIterationSpaceTest PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_lotus_targeted_test(loop_parity_sccdag_attrs_test
               LoopParitySCCDAGAttrsTest.cpp
               LINK_LIBS
                   CanaryLoopAnalysis
                   CanaryCFG
                   CanaryDebugInfo
                   CanaryPDG
                   CanaryAliasAnalysisWrapper
                   LLVMAnalysis)
add_dependencies(loop_parity_sccdag_attrs_test noelle_loop_parity_ll_fixtures)
target_compile_definitions(loop_parity_sccdag_attrs_test PRIVATE
  LOTUS_NOELLE_LOOP_PARITY_FIXTURE_DIR="${NOELLE_LOOP_PARITY_SRC_DIR}"
  LOTUS_NOELLE_LOOP_PARITY_LL_DIR="${NOELLE_LOOP_PARITY_LL_DIR}")
add_test(NAME LoopParitySCCDAGAttrsTest COMMAND ${LOTUS_TEST_BIN_DIR}/loop_parity_sccdag_attrs_test)
set_tests_properties(LoopParitySCCDAGAttrsTest PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
