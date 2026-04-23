add_library(LotusEGraph STATIC
  EGraph.cpp
)

target_include_directories(LotusEGraph
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(LotusEGraph PUBLIC cxx_std_17)
