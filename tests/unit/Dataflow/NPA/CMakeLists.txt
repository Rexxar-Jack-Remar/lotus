# NPA DataFlow Unit Tests

lotus_collect_test_sources(dataflow_tests
  NPASolverTest.cpp
  NPASymmetricAnalysesTest.cpp
  NPAAdaptiveSccTest.cpp
  TensorProductLiftTest.cpp
  NPATensorRegularizationTest.cpp
  Interprocedural/Client/NPAInterproceduralClientStateAndConstantPropagationTest.cpp
  Interprocedural/Client/NPAInterproceduralClientIntervalAndBackwardTest.cpp
  Interprocedural/Client/NPAInterproceduralClientPathTransferTest.cpp
  PredicateRelationDomainTest.cpp
  Interprocedural/Taint/NPAInterproceduralTaintPropagationAndMemoryTest.cpp
  Interprocedural/Taint/NPAInterproceduralTaintCallsAndSinksTest.cpp
  Interprocedural/NPAInterproceduralNullabilityTest.cpp
  NPAThreadSafetyHardeningTest.cpp)
