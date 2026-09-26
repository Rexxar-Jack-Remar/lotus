llvm_map_components_to_libnames(dfa_tooling_llvm_libs IRReader Passes)

# Shared tool-support library for dataflow command-line tools
add_library(CanaryDataflowTooling STATIC ToolSupport.cpp)
target_link_libraries(CanaryDataflowTooling PUBLIC ${dfa_tooling_llvm_libs})