# Examples CMakeLists.txt

set(EXAMPLES
  EGraphExample.cpp
  how-to/build-pdg/PDGExample.cpp
  how-to/query-alias-info/PointerAnalysisExample.cpp
)

set(EXAMPLES_NAMES
  EGraphExample
  PDGExample
  PointerAnalysisExample
)

set(EXAMPLES_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)

# Get LLVM components needed for examples that don't get LLVM through dependencies
llvm_map_components_to_libnames(llvm_libs irreader analysis)

# Set example binary output directory
set(LOTUS_EXAMPLE_BIN_DIR ${CMAKE_BINARY_DIR}/bin/examples)

# Add executables and include directories
foreach(idx RANGE 0 2)
  list(GET EXAMPLES_NAMES ${idx} example_name)
  list(GET EXAMPLES ${idx} example_file)
  add_executable(${example_name} ${example_file})
  set_target_properties(${example_name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${LOTUS_EXAMPLE_BIN_DIR}
  )
  target_include_directories(${example_name} PUBLIC ${EXAMPLES_INCLUDE_DIR})
endforeach()


# PDGExample needs CanaryPDG, AliasAnalysisWrapper (for DataDependencyGraph), and LLVM libs
target_link_libraries(PDGExample PRIVATE 
    CanaryPDG 
    CanaryAliasAnalysisWrapper
    ${llvm_libs}
)

# PointerAnalysisExample needs AliasAnalysisWrapper and all its dependencies
target_link_libraries(PointerAnalysisExample PRIVATE 
    CanaryAliasAnalysisWrapper 
    CanaryUnderApproxAA
    CanaryDyckAA
    CanaryAllocAA
    SeaDsaAnalysis
    ${llvm_libs}
)

