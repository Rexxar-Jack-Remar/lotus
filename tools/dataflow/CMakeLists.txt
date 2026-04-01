# Dataflow testing tools for Elimination, Mono, and IFDS engines.

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

foreach(target lotus-dfa lotus-dfa-apa lotus-dfa-mono lotus-dfa-ifds)
  target_link_libraries(${target} PRIVATE
    APADataFlow
    MONODataFlow
    ${LLVM_LINK_COMPONENTS}
  )
endforeach()

# IFDS-dependent targets need additional linking
target_link_libraries(lotus-dfa PRIVATE IFDS CanaryAliasAnalysisWrapper)
target_link_libraries(lotus-dfa-ifds PRIVATE IFDS CanaryAliasAnalysisWrapper)

# Mono uses AliasAnalysisWrapper (IntraMonoConstantPropagation, IntraMonoUninitVariables)
target_link_libraries(lotus-dfa-mono PRIVATE CanaryAliasAnalysisWrapper)
