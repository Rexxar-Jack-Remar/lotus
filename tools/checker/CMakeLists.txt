# Unified checker front-end.

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Analysis
  IRReader
  Passes
)

add_executable(lotus-check
  lotus_check.cpp
  lotus_kint.cpp
  lotus_taint.cpp
  lotus_concur.cpp
  lotus_pulse.cpp
  lotus_fitx.cpp
  lotus_saber.cpp
  lotus_ae.cpp
  lotus_symex.cpp
)

target_link_libraries(lotus-check PRIVATE
  Annotation
  CanaryAliasAnalysisWrapper
  CanaryFuzzing
  CanaryKint
  CanaryReport
  CanarySymbolicExecution
  CheckerCore
  ConcurrencyChecker
  FiTxChecker
  IFDS
  PulseChecker
  ${LLVM_LINK_COMPONENTS}
  ${Z3_LIBRARIES}
)

# Force linkage of all symbols from Saber and AE libraries so their checker
# registration and option definitions are retained in the merged binary.
if(APPLE)
  target_link_options(lotus-check PRIVATE
    -Wl,-force_load,$<TARGET_FILE:Saber>
    -Wl,-force_load,$<TARGET_FILE:AE>
  )
  target_link_libraries(lotus-check PRIVATE Saber AE)
else()
  target_link_options(lotus-check PRIVATE -Wl,--whole-archive)
  target_link_libraries(lotus-check PRIVATE Saber AE)
  target_link_options(lotus-check PRIVATE -Wl,--no-whole-archive)
endif()
