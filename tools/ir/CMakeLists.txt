add_executable(pdg-query
  pdg-query.cpp
)

add_executable(pdg-slice
  pdg-slice.cpp
)

target_link_libraries(pdg-query
  PRIVATE
  CanaryPDG
  CanaryCUDD
  CanaryAliasAnalysisWrapper
  LLVMIRReader
)

target_link_libraries(pdg-slice
  PRIVATE
  CanaryPDG
  CanaryCUDD
  CanaryAliasAnalysisWrapper
  LLVMIRReader
)
