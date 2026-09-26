add_library(FlowSensitivePTA STATIC
  FlowSensitivePTA.cpp
  ValueFlowGraph.cpp
  ValueFlowPTA.cpp
  VersionedFlowSensitivePTA.cpp)

target_link_libraries(FlowSensitivePTA
  PUBLIC
    SVFG
    CanaryAliasPtsSet)
