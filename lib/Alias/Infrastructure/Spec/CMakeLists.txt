add_library(AliasSpecManager
  AliasSpecManager.cpp
  AliasSpecManagerUtils.cpp
  AliasSpecManagerQueries.cpp
)

# Link against LLVMDemangle since DemangleUtils uses llvm::demangle()
target_link_libraries(AliasSpecManager PUBLIC
  LotusAnnotation
  CanaryLLVMUtils
  LLVMDemangle
)
