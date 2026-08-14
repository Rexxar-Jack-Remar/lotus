# Checker Unit Tests

add_lotus_test_suite(checker_tests
  SOURCES
    Pulse/PulseCheckerTest.cpp
    Pulse/PulseFormulaTest.cpp
    Concurrency/ConcurrencyCheckerTest.cpp
    Concurrency/ConcurrencyCheckerCUDAAndSynchronizationTest.cpp
    Concurrency/ConcurrencyCheckerOpenMPAndMPITest.cpp
    AE/AECheckerTest.cpp
    AE/AECheckerInterproceduralAndBugDetectionTest.cpp
    KINT/KINTCheckerTest.cpp
    Saber/SaberConditionAllocatorTest.cpp
    Saber/SaberConditionAllocatorReportingTest.cpp
    ReportFormattingTest.cpp
    Core/CheckerCoreTest.cpp
  LINK_LIBS
    AE
    CanaryAliasAnalysisWrapper
    PulseChecker
    ConcurrencyChecker
    CanaryKint
    Saber
    CheckerCore
  COMPILE_DEFINITIONS USE_Z3)
