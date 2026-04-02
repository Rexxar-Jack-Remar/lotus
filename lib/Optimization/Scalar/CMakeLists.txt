add_library(CanaryOptimizationScalar STATIC
  AggressiveInliner.cpp
  DeadStoreElimination.cpp
  GVN.cpp
  LICM.cpp
)

target_link_libraries(CanaryOptimizationScalar PUBLIC CanaryLLVMUtils)
