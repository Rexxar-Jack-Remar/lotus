add_library(CanaryPurityAnalysis STATIC
  MemorySSAPuritySummary.cpp
  FunctionPurityAnalysis.cpp
  PurityAttrInferencePass.cpp
)

target_link_libraries(CanaryPurityAnalysis
  PUBLIC
    AliasSpecManager
    CanaryMemorySSA
    #LLVMCore
    LLVMAnalysis
)
