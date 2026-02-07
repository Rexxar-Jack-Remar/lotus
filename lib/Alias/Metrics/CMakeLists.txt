# Metrics over pointer analysis results (precision/soundness comparison)
# See lib/Alias/METRICS.md for design.

add_library(CanaryAliasMetrics STATIC PointerAnalysisMetrics.cpp)
target_link_libraries(CanaryAliasMetrics PUBLIC CanaryAliasAnalysisWrapper)
