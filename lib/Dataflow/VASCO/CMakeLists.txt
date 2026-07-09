# VASCO value-context interprocedural dataflow framework

add_library(VASCODataFlow
  Analyses/LLVMPointsToAnalysis.cpp
  Support/LLVMPointsToMemory.cpp
  Support/LLVMPointsToTypes.cpp)

target_include_directories(VASCODataFlow
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(VASCODataFlow
  PUBLIC
    LLVMCore
    CanaryParallel)
