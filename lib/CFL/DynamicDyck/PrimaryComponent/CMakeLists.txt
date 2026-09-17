# Primary-component refinement solver and its undirected-connectivity backends.
target_sources(CanaryDynamicDyck PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/PrimaryComponentSolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PrimaryConnectivity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SparsifiedConnectivity.cpp)
