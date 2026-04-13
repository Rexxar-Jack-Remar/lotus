add_library(CanaryOptimizationIPO STATIC
  IPDeadStoreElimination.cpp
  IPRedundantLoadElimination.cpp
  IPStoreSinking.cpp
  IPStoreToLoadForwarding.cpp
)

target_link_libraries(CanaryOptimizationIPO PUBLIC CanaryLLVMUtils)

target_include_directories(CanaryOptimizationIPO PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
