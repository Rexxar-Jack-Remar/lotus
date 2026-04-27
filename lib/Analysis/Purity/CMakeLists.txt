add_library(CanaryPurityAnalysis STATIC
  FunctionPurityAnalysis.cpp
)

target_link_libraries(CanaryPurityAnalysis
  PUBLIC
    AliasSpecManager
    #LLVMCore
    LLVMAnalysis
)
