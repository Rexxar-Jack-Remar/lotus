add_library(CanaryOptimizationIPO STATIC
  IPDeadStoreElimination.cpp
  IPRedundantLoadElimination.cpp
  IPStoreSinking.cpp
  IPStoreToLoadForwarding.cpp
)

target_link_libraries(CanaryOptimizationIPO PUBLIC CanaryLLVMUtils)
