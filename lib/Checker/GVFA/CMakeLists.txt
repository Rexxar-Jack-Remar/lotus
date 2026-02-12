# GVFA Vulnerability Checkers (library name GVFAChecker for tools/tests)
set(GVFAChecker_SOURCES
  GVFAVulnerabilityChecker.cpp
  CheckerUtils.cpp
  NullPointerChecker.cpp
  UseAfterFreeChecker.cpp
  UseOfUninitializedVariableChecker.cpp
  InvalidUseOfStackAddressChecker.cpp
  FreeOfNonHeapMemoryChecker.cpp
)

add_library(GVFAChecker STATIC ${GVFAChecker_SOURCES})
target_link_libraries(GVFAChecker
  GVFA
)
