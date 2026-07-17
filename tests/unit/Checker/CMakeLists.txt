# Checker Unit Tests

add_lotus_test_suite(checker_tests
  SOURCES
    Pulse/PulseCheckerTest.cpp
    Pulse/PulseFormulaTest.cpp
    Concurrency/ConcurrencyCheckerTest.cpp
    Concurrency/ConcurrencyCheckerTestPart2.cpp
    Concurrency/ConcurrencyCheckerTestPart3.cpp
    AE/AECheckerTest.cpp
    AE/AECheckerTestPart2.cpp
    KINT/KINTCheckerTest.cpp
    Saber/SaberConditionAllocatorTest.cpp
    Saber/SaberConditionAllocatorTestPart2.cpp
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
