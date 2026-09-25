# Dataflow testing tools for Elimination, Mono, IFDS, NPA, and WPDS engines.

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  IRReader
  Passes
)

# Main unified tool
add_executable(lotus-dfa lotus-dfa-diff.cpp)

# Engine-specific tools
add_executable(lotus-dfa-apa lotus-dfa-apa.cpp)
add_executable(lotus-dfa-mono lotus-dfa-mono.cpp)
add_executable(lotus-dfa-ifds lotus-dfa-ifds.cpp)
add_executable(lotus-dfa-npa lotus-dfa-npa.cpp)
add_executable(lotus-dfa-wpds lotus-dfa-wpds.cpp)

foreach(target lotus-dfa lotus-dfa-apa lotus-dfa-mono lotus-dfa-ifds)
  target_link_libraries(${target} PRIVATE
    APADataFlow
    MONODataFlow
    CanaryDataflowTooling
    ${LLVM_LINK_COMPONENTS}
  )
endforeach()

target_link_libraries(lotus-dfa-npa PRIVATE
  NPADataFlow
  CanaryAliasAnalysisWrapper
  CanaryDataflowTooling
  ${LLVM_LINK_COMPONENTS}
)

target_link_libraries(lotus-dfa-wpds PRIVATE
  WPDSDataFlow
  wpds++
  CanaryDataflowTooling
  ${LLVM_LINK_COMPONENTS}
)

# IFDS-dependent targets need additional linking
target_link_libraries(lotus-dfa PRIVATE IFDS CanaryAliasAnalysisWrapper CanaryDataflowTooling)
target_link_libraries(lotus-dfa-ifds PRIVATE IFDS CanaryAliasAnalysisWrapper CanaryDataflowTooling)

# Mono uses AliasAnalysisWrapper (IntraMonoConstantPropagation, IntraMonoUninitVariables)
target_link_libraries(lotus-dfa-mono PRIVATE CanaryAliasAnalysisWrapper CanaryDataflowTooling)

if(LOTUS_ENABLE_DEMAND_APA)
  add_subdirectory(DemandAPA)
endif()
