# POPL 2022 weighted-quotient solver and its original artifact file workflow.
target_sources(CanaryDynamicDyck PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/WeightedQuotientSolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CycleDeletion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DotParser.cpp)
