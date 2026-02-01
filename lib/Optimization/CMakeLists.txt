add_subdirectory(PE)

add_library(CanaryOptimization STATIC
  AInliner.cpp
  ModuleOptimizer.cpp
  SWPrefetching.cpp
  LICM.cpp
  IP/IPDeadStoreElimination.cpp
  IP/IPRedundantLoadElimination.cpp
  IP/IPStoreSinking.cpp
  IP/IPStoreToLoadForwarding.cpp
)
target_link_libraries(CanaryOptimization CanaryLLVMUtils)
