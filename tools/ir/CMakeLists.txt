add_executable(lotus-ir-pdg-query
  lotus-ir-pdg-query.cpp
)

target_link_libraries(lotus-ir-pdg-query
  PRIVATE
  CanaryPDG
  CanaryCUDD
  CanaryAliasAnalysisWrapper
  LLVMIRReader
)
