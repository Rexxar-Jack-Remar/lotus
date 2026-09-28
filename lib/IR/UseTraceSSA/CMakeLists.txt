add_library(CanaryUseTraceSSACore STATIC
  UseTraceSSA.cpp
  TraceFlowGraph.cpp
  Query.cpp
  SVFGImporter.cpp
  ResourceHistory.cpp
  Models.cpp
  DefectDetector.cpp
)
set_target_properties(CanaryUseTraceSSACore PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The API reports invalid construction through C++ exceptions. Do not inherit
# LLVM's optional -fno-exceptions setting in this component or its consumers.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(CanaryUseTraceSSACore PUBLIC -fexceptions)
endif()

add_library(CanaryUseTraceSSA STATIC LLVMImporter.cpp)
target_link_libraries(CanaryUseTraceSSA PUBLIC CanaryUseTraceSSACore LLVMCore LLVMAnalysis LLVMSupport)
set_target_properties(CanaryUseTraceSSA PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(CanaryUseTraceSSASVFG STATIC SVFGBridge.cpp)
target_link_libraries(CanaryUseTraceSSASVFG PUBLIC CanaryUseTraceSSA SVFG)

if(LOTUS_BUILD_TESTS)
  add_executable(lotus-usetracessa-core-test
    ${CMAKE_SOURCE_DIR}/tests/unit/IR/UseTraceSSA/HistoryTest.cpp)
  target_link_libraries(lotus-usetracessa-core-test PRIVATE CanaryUseTraceSSACore)
  foreach(_case straight-line repeated-operands paper-example diamond pruned-phi
      loop definition-reset edge-uses parallel-edges invoke-definition unreachable
      traps bypass-trap invalid-input empty-unused deterministic-output
      irreducible loop-phi-operands randomized-histories deep-cfg)
    add_test(NAME UseTraceSSA.Core.${_case} COMMAND lotus-usetracessa-core-test ${_case})
  endforeach()

  add_executable(lotus-usetracessa-llvm-test
    ${CMAKE_SOURCE_DIR}/tests/unit/IR/UseTraceSSA/LLVMHistoryTest.cpp)
  target_link_libraries(lotus-usetracessa-llvm-test PRIVATE CanaryUseTraceSSA LLVMAsmParser)
  add_test(NAME UseTraceSSA.LLVM COMMAND lotus-usetracessa-llvm-test)

  add_executable(lotus-usetracessa-flow-test
    ${CMAKE_SOURCE_DIR}/tests/unit/IR/UseTraceSSA/FlowTest.cpp)
  target_link_libraries(lotus-usetracessa-flow-test PRIVATE CanaryUseTraceSSACore)
  foreach(_case traps bypass weak-trap object-guards svfg-ordering invalid-import
      native-contract paper-resources no-alias-union unknown-alias loop-reset
      loop-double-free use-after-free unallocated-resource matching-calls realizable-segments nested-calls
      balanced-cycle recursion resource-call callee-trap limits threads ordered-events
      no-address-taint unknown-library missing-memory-ports
      custom-model no-memset-kill slices serialization invalid-queries multi-source
      random-dyck auto-builder auto-memory-phi auto-call defect-detector)
    add_test(NAME UseTraceSSA.Flow.${_case} COMMAND lotus-usetracessa-flow-test ${_case})
  endforeach()
endif()
