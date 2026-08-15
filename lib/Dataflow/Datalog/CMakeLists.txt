add_library(LotusDatalog STATIC
  Context.cpp
  Rule.cpp
  SemanticProgram.cpp
  Analysis/Analyzer.cpp
  Planner/Planner.cpp
  Runtime/Executor.cpp
  Runtime/RelationStore.cpp
  Runtime/Scheduler.cpp)

target_include_directories(LotusDatalog PUBLIC
  ${PROJECT_SOURCE_DIR}/include)
target_include_directories(LotusDatalog PRIVATE
  ${PROJECT_SOURCE_DIR}/lib)

set_target_properties(LotusDatalog PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
