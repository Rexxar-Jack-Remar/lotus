set(PulseChecker_SOURCES
  PulseDomain.cpp
  PulseOperations.cpp
  PulseChecker.cpp
  PulseFormula.cpp
  PulseSummary.cpp
  PulseSubstitution.cpp
  PulseSummaryApplication.cpp
  PulseDisjunctiveDomain.cpp
  PulseLoopAbstraction.cpp
  PulseLatentIssue.cpp
  PulseNonDisjunctiveDomain.cpp
  PulseTransitiveInfo.cpp
  PulseContradiction.cpp
  PulseTaint.cpp
  PulseModels.cpp
  PulseDiagnostic.cpp
  PulseReport.cpp
  PulseSpecialization.cpp
  PulseJoin.cpp
  PulseCallState.cpp
  PulseLogger.cpp
)

add_library(PulseChecker STATIC ${PulseChecker_SOURCES})

target_link_libraries(PulseChecker
  CanaryReport
  CanaryAliasAnalysisWrapper
  ${Z3_LIBRARIES}
)
