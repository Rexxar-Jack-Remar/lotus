# Sifa (Symbolic Interpretation with Fluid Abstractions) verifier tool

add_executable(lotus-verify-sifa lotus-verify-sifa.cpp)

target_link_libraries(lotus-verify-sifa PRIVATE
  SifaAnalysis
  CanaryAliasAnalysisWrapper
  ${Z3_LIBRARIES}
)

if(APPLE)
  target_link_options(lotus-verify-sifa PRIVATE -Wl,-all_load)
endif()

install(TARGETS lotus-verify-sifa DESTINATION bin)
