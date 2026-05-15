add_executable(pdg-query
  lotus-ir-pdg-query.cpp
)

target_link_libraries(pdg-query
  PRIVATE
  CanaryPDG
  CanaryCUDD
  CanaryAliasAnalysisWrapper
  LLVMIRReader
)
