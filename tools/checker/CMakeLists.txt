# Bug checking tools: gvfa, kint, taint, and concur

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Coroutines
  Target
  #Passes
)

# Global Value Flow Analysis Tool (unified, includes null pointer analysis)
add_executable(lotus-gvfa lotus_gvfa.cpp)
target_link_libraries(lotus-gvfa PRIVATE
  GVFAChecker
  CanaryReport
  Annotation
  CanaryDyckAA
  CanaryNullPointer
  ${LLVM_LINK_COMPONENTS}
)

# Kint: A Bug-Finding Tool for C Programs
set(LLVM_LINK_COMPONENTS_KINT
        LLVMPasses)

add_executable(lotus-kint lotus_kint.cpp)
target_link_libraries(lotus-kint PRIVATE
        CanaryKint
        CanaryReport
        CanarySupport
        ${LLVM_LINK_COMPONENTS_KINT}
        ${Z3_LIBRARIES} 
)

# IFDS Taint Analysis Tool (formerly llvm-ai)
add_executable(lotus-taint
  lotus_taint.cpp
  lotus_taint_microbench.cpp
)
target_link_libraries(lotus-taint
  IFDS
  Annotation
  CanaryAliasAnalysisWrapper
)

# Concurrency Checker Tool
add_executable(lotus-concur lotus_concur.cpp)
target_link_libraries(lotus-concur
  ConcurrencyChecker
  CanaryReport
)

# Pulse Bug Finder Tool
add_executable(lotus-pulse lotus_pulse.cpp)
target_link_libraries(lotus-pulse PRIVATE
  PulseChecker
  CanaryAliasAnalysisWrapper
  CanaryReport
  ${LLVM_LINK_COMPONENTS}
  ${Z3_LIBRARIES}
)

# FiTx Bug Finder Tool (runs all FiTx checkers: df, dl, dul, leak, ref_count, uaf, etc.)
add_executable(lotus-fitx lotus_fitx.cpp)
target_link_libraries(lotus-fitx PRIVATE
  FiTxChecker
  ${LLVM_LINK_COMPONENTS}
)
# LoopInfoWrapperPass and legacy pass manager for running FiTx pipeline
llvm_map_components_to_libnames(_fitx_components Analysis Passes)
target_link_libraries(lotus-fitx PRIVATE ${_fitx_components})