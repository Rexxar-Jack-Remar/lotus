# Failure-directed program trimming (FSE'17) — split implementation

add_library(FailureDirectedTrimming STATIC
  Options.cpp
  Expr.cpp
  Codegen.cpp
  Z3QE.cpp
  SafetyConditions.cpp
  CloneAndWrap.cpp
  Pass.cpp
)

target_include_directories(FailureDirectedTrimming
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)


target_link_libraries(FailureDirectedTrimming
  PUBLIC
    CanaryAliasAnalysisWrapper
    ${Z3_LIBRARIES}
)
