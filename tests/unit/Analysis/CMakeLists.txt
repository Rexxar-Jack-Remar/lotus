# Analysis Unit Tests

set(NOELLE_LOOP_PARITY_SRC_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/noelle_parity)
set(NOELLE_LOOP_PARITY_LL_DIR
    ${CMAKE_BINARY_DIR}/tests/unit/noelle_loop_parity_ll)

file(MAKE_DIRECTORY ${NOELLE_LOOP_PARITY_LL_DIR})

if(APPLE)
  if(EXISTS /opt/homebrew/opt/llvm@14/bin/clang++)
    set(NOELLE_LOOP_PARITY_CLANGXX /opt/homebrew/opt/llvm@14/bin/clang++)
  elseif(EXISTS /usr/local/opt/llvm@14/bin/clang++)
    set(NOELLE_LOOP_PARITY_CLANGXX /usr/local/opt/llvm@14/bin/clang++)
  else()
    execute_process(
        COMMAND xcrun --sdk macosx --find clang++
        OUTPUT_VARIABLE NOELLE_LOOP_PARITY_CLANGXX
        OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()
  if(EXISTS /opt/homebrew/opt/llvm@14/bin/opt)
    set(NOELLE_LOOP_PARITY_OPT /opt/homebrew/opt/llvm@14/bin/opt)
  elseif(EXISTS /usr/local/opt/llvm@14/bin/opt)
    set(NOELLE_LOOP_PARITY_OPT /usr/local/opt/llvm@14/bin/opt)
  else()
    find_program(NOELLE_LOOP_PARITY_OPT opt REQUIRED)
  endif()
  execute_process(
      COMMAND xcrun --sdk macosx --show-sdk-path
      OUTPUT_VARIABLE NOELLE_LOOP_PARITY_SYSROOT
      OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(NOELLE_LOOP_PARITY_EXTRA_ARGS -isysroot ${NOELLE_LOOP_PARITY_SYSROOT})
else()
  find_program(NOELLE_LOOP_PARITY_CLANGXX
               NAMES clang++-14 clang++
               HINTS ${LOTUS_LLVM_TOOLS_BINARY_DIR})
  if(NOT NOELLE_LOOP_PARITY_CLANGXX)
    set(NOELLE_LOOP_PARITY_CLANGXX ${CMAKE_CXX_COMPILER})
  endif()
  find_program(NOELLE_LOOP_PARITY_OPT
               NAMES opt-14 opt
               HINTS ${LOTUS_LLVM_TOOLS_BINARY_DIR}
               REQUIRED)
  set(NOELLE_LOOP_PARITY_EXTRA_ARGS)
endif()

set(NOELLE_LOOP_PARITY_CASES
    iv_attributes/nested_loop_governing
    sccdag_attributes/simple
    sccdag_attributes/reducible_addition
    sccdag_attributes/clonable_alloca
    loop_domain_space/one_dim_array
    loop_domain_space/two_dim_matrix)

set(NOELLE_LOOP_PARITY_LL_OUTPUTS)
foreach(case_name ${NOELLE_LOOP_PARITY_CASES})
  set(src_file ${NOELLE_LOOP_PARITY_SRC_DIR}/${case_name}/test.cpp)
  string(REPLACE "/" "_" case_output ${case_name})
  set(pre_bc ${NOELLE_LOOP_PARITY_LL_DIR}/${case_output}_pre.bc)
  set(ll_file ${NOELLE_LOOP_PARITY_LL_DIR}/${case_output}.ll)
  list(APPEND NOELLE_LOOP_PARITY_LL_OUTPUTS ${ll_file})
  add_custom_command(
      OUTPUT ${ll_file}
      COMMAND ${NOELLE_LOOP_PARITY_CLANGXX}
              ${NOELLE_LOOP_PARITY_EXTRA_ARGS}
              -std=c++17 -emit-llvm -c -g -O0 -Xclang -disable-O0-optnone
              ${src_file} -o ${pre_bc}
      COMMAND ${NOELLE_LOOP_PARITY_OPT}
              -basic-aa -mem2reg -scalar-evolution -loops
              -loop-simplify -lcssa -domtree -postdomtree
              -S ${pre_bc} -o ${ll_file}
      DEPENDS ${src_file}
      COMMENT "Generating LLVM IR for NOELLE parity fixture ${case_name}"
      VERBATIM)
endforeach()

add_custom_target(noelle_loop_parity_ll_fixtures DEPENDS ${NOELLE_LOOP_PARITY_LL_OUTPUTS})

add_lotus_debug_info_test(debug_info_analysis_test DebugInfoAnalysisTest.cpp)
add_lotus_debug_info_test(unique_ir_marker_test UniqueIRMarkerTest.cpp)
add_lotus_targeted_test(hot_profile_test
               HotProfileTest.cpp
               LINK_LIBS
                   CanaryProfile
                   LLVMAnalysis)
add_lotus_analysis_test(nullpointer_api_test NullPointerAPITest.cpp)
add_lotus_analysis_test(nullpointer_legacy_helpers_test NullPointerLegacyHelpersTest.cpp)
add_lotus_analysis_test(context_sensitive_nullcheck_analysis_test
               ContextSensitiveNullCheckAnalysisTest.cpp)
add_lotus_targeted_test(symbolic_execution_spec_compat_test
               SymbolicExecutionSpecCompatTest.cpp
               LINK_LIBS
                   CanarySymbolicExecution
                   CanaryGuardedValueFlow
                   LLVMAnalysis)
add_lotus_targeted_test(symbolic_execution_taint_model_test
               SymbolicExecutionTaintModelTest.cpp
               LINK_LIBS
                   CanarySymbolicExecution
                   CanaryGuardedValueFlow
                   LLVMAnalysis)
add_lotus_spectre_test(spectre_analysis_test SpectreAnalysisTest.cpp)
add_lotus_crypto_test(crypto_analysis_test CryptoAnalysisTest.cpp)
add_lotus_targeted_test(prue_pass_test
               PruePassTest.cpp
               LINK_LIBS
                   CanaryNisse
                   LLVMAnalysis
                   LLVMTransformUtils)
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
