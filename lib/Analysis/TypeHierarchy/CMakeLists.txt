set(CanaryTypeHierarchy_SOURCES
  TypeHierarchyAnalysis.cpp
  DIBasedTypeHierarchy.cpp
  DIBasedTypeHierarchyData.cpp
  LLVMVFTable.cpp
  LLVMVFTableData.cpp
)
add_library(CanaryTypeHierarchy STATIC ${CanaryTypeHierarchy_SOURCES})
target_link_libraries(CanaryTypeHierarchy CanaryLLVMUtils)

# Suppress deprecation warnings from third-party libraries (spdlog/fmt)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(CanaryTypeHierarchy PRIVATE -Wno-deprecated-declarations)
endif()
