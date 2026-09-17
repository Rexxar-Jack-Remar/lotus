add_library(CanaryDynamicDyck STATIC
    Graph.cpp
    IO.cpp)
target_include_directories(CanaryDynamicDyck PUBLIC ${PROJECT_SOURCE_DIR}/include)

add_subdirectory(WeightedQuotient)
add_subdirectory(PrimaryComponent)
