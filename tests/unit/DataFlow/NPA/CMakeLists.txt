# NPA DataFlow Unit Tests

add_lotus_test(npa_solver_test NPASolverTest.cpp)
add_lotus_test(npa_tensor_domain_test TensorProductDomainTest.cpp)
add_lotus_test(npa_tensor_regularization_test NPATensorRegularizationTest.cpp)
add_lotus_test(npa_interprocedural_client_test NPAInterproceduralClientTest.cpp)
add_lotus_test(npa_affine_relation_domain_test AffineRelationDomainTest.cpp)
add_lotus_test(npa_predicate_relation_domain_test PredicateRelationDomainTest.cpp)
add_lotus_test(npa_interprocedural_taint_test NPAInterproceduralTaintTest.cpp)
add_lotus_test(npa_interprocedural_nullability_test NPAInterproceduralNullabilityTest.cpp)
add_lotus_test(npa_thread_safety_hardening_test NPAThreadSafetyHardeningTest.cpp)

set(NPA_PARALLEL_RHS_HARNESS_LIBS ${COMMON_LIBS})
list(REMOVE_ITEM NPA_PARALLEL_RHS_HARNESS_LIBS GTest::gtest_main)

add_executable(npa_parallel_rhs_harness_test NPAParallelRhsHarnessTest.cpp)
set_target_properties(npa_parallel_rhs_harness_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
target_include_directories(npa_parallel_rhs_harness_test PRIVATE ${COMMON_INCLUDES})
target_link_libraries(npa_parallel_rhs_harness_test ${NPA_PARALLEL_RHS_HARNESS_LIBS})

add_test(NAME npa_parallel_rhs_harness_serial_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/npa_parallel_rhs_harness_test -nworkers=0)
set_tests_properties(npa_parallel_rhs_harness_serial_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME npa_parallel_rhs_harness_parallel_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/npa_parallel_rhs_harness_test -nworkers=2)
set_tests_properties(npa_parallel_rhs_harness_parallel_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
