add_library(CanaryAliasPtsSet STATIC
  BDDPtsSet.cpp
)

target_link_libraries(CanaryAliasPtsSet
  PUBLIC CanaryCUDD
)
