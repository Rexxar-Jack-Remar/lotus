cmake_minimum_required(VERSION 3.10)

# Usable both through lib/IR/add_subdirectory(UseHistory) and independently.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(CanaryUseHistory LANGUAGES C CXX)
  set(LOTUS_USEHISTORY_STANDALONE ON)
  include(CTest)
endif()

get_filename_component(LOTUS_USEHISTORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
option(LOTUS_USEHISTORY_WITH_LLVM "Build the LLVM adapter, passes, and LLVM tests" ON)
if(LOTUS_USEHISTORY_STANDALONE)
  set(_usehistory_tests_default ${BUILD_TESTING})
else()
  set(_usehistory_tests_default ${LOTUS_BUILD_TESTS})
endif()
option(LOTUS_USEHISTORY_BUILD_TESTS "Build UseHistory tests (no GTest dependency)" ${_usehistory_tests_default})

add_library(CanaryUseHistoryCore STATIC UseHistory.cpp FlowGraph.cpp Query.cpp
  SVFGAdapter.cpp SVFGHistoryBuilder.cpp ResourceHistory.cpp Models.cpp
  DefectDetector.cpp ReachabilityIndex.cpp)
target_compile_features(CanaryUseHistoryCore PUBLIC cxx_std_17)
target_include_directories(CanaryUseHistoryCore PUBLIC "${LOTUS_USEHISTORY_ROOT}/include")
set_target_properties(CanaryUseHistoryCore PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(LOTUS_USEHISTORY_WITH_LLVM)
  if(NOT LLVM_FOUND)
    find_package(LLVM REQUIRED CONFIG)
  endif()
  if(LLVM_VERSION_MAJOR LESS 14)
    message(FATAL_ERROR "Lotus UseHistory requires LLVM 14 or newer")
  endif()
  add_library(CanaryUseHistory STATIC LLVMHistory.cpp LLVMFlow.cpp HistoryPass.cpp)
  target_include_directories(CanaryUseHistory SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
  separate_arguments(_usehistory_llvm_definitions NATIVE_COMMAND "${LLVM_DEFINITIONS}")
  target_compile_options(CanaryUseHistory PRIVATE ${_usehistory_llvm_definitions})
  target_link_libraries(CanaryUseHistory PUBLIC CanaryUseHistoryCore LLVMCore LLVMAnalysis LLVMSupport)
  set_target_properties(CanaryUseHistory PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(Lotus::UseHistory ALIAS CanaryUseHistory)
  if(NOT LOTUS_USEHISTORY_STANDALONE AND TARGET SVFG)
    add_library(CanaryUseHistorySVFG STATIC LotusSVFG.cpp)
    target_link_libraries(CanaryUseHistorySVFG PUBLIC CanaryUseHistory SVFG)
    add_library(Lotus::UseHistorySVFG ALIAS CanaryUseHistorySVFG)
  endif()
endif()

if(LOTUS_USEHISTORY_BUILD_TESTS)
  enable_testing()
  add_executable(lotus-usehistory-core-test ${LOTUS_USEHISTORY_ROOT}/tests/unit/IR/UseHistory/HistoryTest.cpp)
  target_link_libraries(lotus-usehistory-core-test PRIVATE CanaryUseHistoryCore)
  foreach(_case straight-line repeated-operands paper-example diamond pruned-phi
      loop definition-reset edge-uses parallel-edges invoke-definition unreachable
      traps bypass-trap invalid-input empty-unused deterministic-output
      irreducible loop-phi-operands randomized-histories deep-cfg)
    add_test(NAME UseHistory.Core.${_case} COMMAND lotus-usehistory-core-test ${_case})
  endforeach()
  if(LOTUS_USEHISTORY_WITH_LLVM)
    add_executable(lotus-usehistory-llvm-test ${LOTUS_USEHISTORY_ROOT}/tests/unit/IR/UseHistory/LLVMHistoryTest.cpp)
    target_link_libraries(lotus-usehistory-llvm-test PRIVATE CanaryUseHistory LLVMAsmParser)
    add_test(NAME UseHistory.LLVM COMMAND lotus-usehistory-llvm-test)
  endif()
endif()

if(LOTUS_USEHISTORY_BUILD_TESTS)
  add_executable(lotus-usehistory-flow-test ${LOTUS_USEHISTORY_ROOT}/tests/unit/IR/UseHistory/FlowTest.cpp)
  target_link_libraries(lotus-usehistory-flow-test PRIVATE CanaryUseHistoryCore)
  foreach(_case traps bypass weak-trap object-guards svfg-ordering invalid-import
      native-contract paper-resources no-alias-union unknown-alias loop-reset
      loop-double-free use-after-free unallocated-resource matching-calls realizable-segments nested-calls
      balanced-cycle recursion resource-call callee-trap limits threads ordered-events
      library-heartbleed no-address-taint unknown-library missing-memory-ports
      custom-model no-memset-kill slices serialization invalid-queries multi-source
      random-dyck auto-builder auto-memory-phi auto-call reachability-index defect-detector)
    add_test(NAME UseHistory.Flow.${_case} COMMAND lotus-usehistory-flow-test ${_case})
  endforeach()
endif()

# The API reports invalid construction through C++ exceptions. Do not inherit
# LLVM's optional -fno-exceptions setting in this component or its consumers.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(CanaryUseHistoryCore PUBLIC -fexceptions)
endif()
add_library(Lotus::UseHistoryCore ALIAS CanaryUseHistoryCore)
