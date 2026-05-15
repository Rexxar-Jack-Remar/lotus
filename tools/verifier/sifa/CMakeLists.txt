# Sifa (Symbolic Interpretation with Fluid Abstractions) verifier tool

add_executable(sifa lotus-verify-sifa.cpp)

target_link_libraries(sifa PRIVATE
  SifaAnalysis
  CanaryAliasAnalysisWrapper
  ${Z3_LIBRARIES}
)

if(APPLE)
  target_link_options(sifa PRIVATE -Wl,-all_load)
endif()

install(TARGETS sifa DESTINATION bin)
