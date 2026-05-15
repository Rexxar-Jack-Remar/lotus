set(ConstantTimeVerify_SOURCES
  ConstantTimePass.cpp
  ConstantTimeAnalysis.cpp
  ConstantTimeDebugInfo.cpp
  ConstantTimeInlining.cpp
  ConstantTimeReporting.cpp
  ConstantTimeTargets.cpp
)
add_library(ConstantTimeVerify STATIC ${ConstantTimeVerify_SOURCES})
