# Dataflow diff testing tool: compare results of Elimination vs Mono (and optionally WPDS) engines.

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Core
  IRReader
  Passes
  Support
)

add_executable(lotus-dfa-diff lotus-dfa-diff.cpp)

target_link_libraries(lotus-dfa-diff PRIVATE
  EliminationDataFlow
  MONODataFlow
  ${LLVM_LINK_COMPONENTS}
)
