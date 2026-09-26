add_executable(lotus-demand-apa Main.cpp)
target_link_libraries(lotus-demand-apa PRIVATE DemandAPA)

add_executable(lotus-demand-apa-bdd-playground EXCLUDE_FROM_ALL BddPlayground.cpp)
target_link_libraries(lotus-demand-apa-bdd-playground PRIVATE DemandAPA)
