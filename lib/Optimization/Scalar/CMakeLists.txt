add_library(CanaryOptimizationScalar STATIC
  AggressiveInliner.cpp
  LICM.cpp
)

target_link_libraries(CanaryOptimizationScalar PUBLIC CanaryLLVMUtils)
