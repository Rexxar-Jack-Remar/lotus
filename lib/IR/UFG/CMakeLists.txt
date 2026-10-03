add_library(CanaryUFG STATIC UFG.cpp Search.cpp DefectDetector.cpp)
target_link_libraries(CanaryUFG PUBLIC CanaryUseTraceSSACore)
set_target_properties(CanaryUFG PROPERTIES POSITION_INDEPENDENT_CODE ON)
