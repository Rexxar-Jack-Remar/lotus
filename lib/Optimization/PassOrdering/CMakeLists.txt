add_library(CanaryOptimizationPipeline STATIC
  ModuleOptimizer.cpp
)

target_link_libraries(CanaryOptimizationPipeline PUBLIC CanaryLLVMUtils)
