# NPA DataFlow Unit Tests

lotus_collect_test_sources(dataflow_tests
  NPASolverTest.cpp
  NPASymmetricAnalysesTest.cpp
  NPAAdaptiveSccTest.cpp
  TensorProductLiftTest.cpp
  NPATensorRegularizationTest.cpp
  Interprocedural/Client/NPAInterproceduralClientTest.cpp
  PredicateRelationDomainTest.cpp
  Interprocedural/Taint/NPAInterproceduralTaintTest.cpp
  Interprocedural/NPAInterproceduralNullabilityTest.cpp
  NPAThreadSafetyHardeningTest.cpp)
