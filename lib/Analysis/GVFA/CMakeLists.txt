set(GVFA_SOURCES
  GlobalValueFlowAnalysis.cpp
  GVFAEngine.cpp
  FastGVFAEngine.cpp
  HybridGVFAEngine.cpp
  PreciseGVFAEngine.cpp
  GVFAUtils.cpp
)
add_library(GVFA STATIC ${GVFA_SOURCES})
target_link_libraries(GVFA CanaryDyckAA Annotation)
