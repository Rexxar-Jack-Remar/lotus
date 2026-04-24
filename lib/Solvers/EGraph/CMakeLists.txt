add_library(LotusEGraph STATIC
  EGraph.cpp
)

target_link_libraries(LotusEGraph
  PUBLIC
   # ${Z3_LIBRARIES}
)
