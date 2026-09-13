# APA DataFlow Unit Tests

add_lotus_test_suite(apa_tests
  SOURCES
    EliminationGraphSolverTest.cpp
    EliminationADTTest.cpp
    EliminationLLVMIntraTest.cpp
    EliminationInterproceduralTest.cpp
    InterStandardAnalysesTest.cpp
    AffineRelationDomainTest.cpp
    AffineHardeningTest.cpp
    StandardDomainTest.cpp
    InterAffineCallAndControlFlowTest.cpp
    InterAffinePredicateTest.cpp
    InterAffineCastAndArithmeticTest.cpp
    InterAffineBitwiseCongruenceTest.cpp
    InterSummaryTransferTest.cpp
    CallGraphSCCTest.cpp
    ModularInterSummarySolverTest.cpp
    ModularInterSummaryDriverTest.cpp
    PathSummaryEquationSolverTest.cpp
    OrderTest.cpp
    EAN/RoundtripTest.cpp
    EAN/FactorizeTest.cpp
    EAN/SaturateTest.cpp
    EAN/BatchExtractTest.cpp
    EAN/StarTest.cpp
    EAN/SolverWiringTest.cpp
    EAN/EvalRq1Test.cpp
    EAN/EvalRq3Test.cpp
    EAN/GreedyTest.cpp
    EAN/InterWiringTest.cpp
    EAN/AffineEanTest.cpp
    EAN/ExpandTest.cpp
    EAN/EvalExportTest.cpp
    Baseline/TranslAPAGenKillTest.cpp
    Baseline/TranslAPADriverTest.cpp
  LINK_LIBS APADataFlow LotusEGraph)
