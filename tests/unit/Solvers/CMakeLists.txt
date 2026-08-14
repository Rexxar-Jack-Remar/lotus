set(LOTUS_SOLVER_TEST_SOURCES
    SymAbsTest.cpp
    EGraphCoreTest.cpp
    EGraphSimpleTest.cpp
    EGraphMathTest.cpp
    EGraphPatternAndRewriteTest.cpp
    EGraphExplanationTest.cpp
    EGraphExtractorAndAnalysisTest.cpp
    EGraphSerializationAndSetOpsTest.cpp)
set(LOTUS_SOLVER_TEST_LIBS
    SymAbs
    LotusEGraph
    ${Z3_LIBRARIES})
set(LOTUS_SOLVER_TEST_DEFINITIONS)

if(LOTUS_ENABLE_SMT_STABILIZER)
  list(APPEND LOTUS_SOLVER_TEST_SOURCES SMTStabilizerTest.cpp)
  list(APPEND LOTUS_SOLVER_TEST_LIBS LotusSMTStabilizer)
  list(APPEND LOTUS_SOLVER_TEST_DEFINITIONS
      "SMT_STABILIZER_TEST_INPUT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/SMTStabilizerInputs\"")
endif()

add_lotus_test_suite(solver_tests
  SOURCES ${LOTUS_SOLVER_TEST_SOURCES}
  LINK_LIBS ${LOTUS_SOLVER_TEST_LIBS}
  COMPILE_DEFINITIONS ${LOTUS_SOLVER_TEST_DEFINITIONS})
