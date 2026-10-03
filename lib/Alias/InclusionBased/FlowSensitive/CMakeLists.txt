add_library(FlowSensitivePTA STATIC
  Sparse/FlowSensitivePTA.cpp
  Parallel/ParallelFlowSensitivePTA.cpp
  Parallel/OrderedFlowSensitivePTA.cpp
  Versioned/VersionedFlowSensitivePTA.cpp
  ValueFlow/ValueFlowGraph.cpp
  ValueFlow/ValueFlowPTA.cpp)

target_link_libraries(FlowSensitivePTA
  PUBLIC
    SVFG
    CanaryAliasPtsSet)

find_package(Threads REQUIRED)
target_link_libraries(FlowSensitivePTA PRIVATE Threads::Threads)
