add_library(CanaryDynamicDyck STATIC
    Graph.cpp
    Solver.cpp
    IO.cpp
    Detail/Engine.cpp
    Detail/CycleDeletion.cpp
    Detail/DotParser.cpp)
target_include_directories(CanaryDynamicDyck PUBLIC ${PROJECT_SOURCE_DIR}/include)
