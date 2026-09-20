add_library(CanaryGPG STATIC
  Analysis.cpp
  Graph.cpp
  GPU.cpp
  IndirectionList.cpp
  InternedReaching.cpp
  LLVMFrontend.cpp
  ProgramModel.cpp
  Result.cpp
)

target_link_libraries(CanaryGPG PUBLIC
  LLVMAnalysis
  LLVMCore
  LLVMSupport
)
