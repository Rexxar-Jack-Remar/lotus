add_library(LotusEGraph STATIC
  EGraph.cpp
)

if(LOTUS_EGRAPH_ENABLE_DOT)
  target_compile_definitions(LotusEGraph PUBLIC LOTUS_EGRAPH_ENABLE_DOT=1)
else()
  target_compile_definitions(LotusEGraph PUBLIC LOTUS_EGRAPH_ENABLE_DOT=0)
endif()

if(LOTUS_EGRAPH_ENABLE_JSON)
  target_compile_definitions(LotusEGraph PUBLIC LOTUS_EGRAPH_ENABLE_JSON=1)
else()
  target_compile_definitions(LotusEGraph PUBLIC LOTUS_EGRAPH_ENABLE_JSON=0)
endif()

target_link_libraries(LotusEGraph
  PUBLIC
    $<$<BOOL:${LOTUS_EGRAPH_ENABLE_JSON}>:CanaryFormats>
   # ${Z3_LIBRARIES}
)
