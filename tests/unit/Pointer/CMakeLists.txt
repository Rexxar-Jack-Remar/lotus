# Pointer Analysis Unit Tests

# Alias Analysis Tests
add_lotus_test(alloc_aa_test AllocAATest.cpp)
add_lotus_test(aser_pta_test AserPTATest.cpp)
add_lotus_test(dda_aa_test DDAATest.cpp)
add_lotus_test(dyck_aa_test DyckAATest.cpp)
add_lotus_test(sparrow_aa_test SparrowAATest.cpp)
add_lotus_test(tpa_test TPATest.cpp)
add_lotus_test(underapprox_aa_test UnderApproxAATest.cpp)
add_lotus_test(alias_graph_test AliasGraphTest.cpp)
add_lotus_test(dfpa_test DFPATest.cpp)
add_lotus_test(lotus_aa_test LotusAATest.cpp)

if(TARGET TypeQualifier)
  add_executable(type_qualifier_test ${CMAKE_CURRENT_SOURCE_DIR}/TypeQualifierTest.cpp)
  set_target_properties(type_qualifier_test PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
      CXX_STANDARD 17
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF
  )
  target_include_directories(type_qualifier_test PRIVATE ${COMMON_INCLUDES})
  target_link_libraries(type_qualifier_test PRIVATE
      GTest::gtest
      GTest::gtest_main
      ${LLVM_LIBRARIES}
      LLVMAsmParser
      Annotation
      CanarySupport
      AliasSpecManager
      TypeQualifier
  )
  add_test(NAME type_qualifier_test COMMAND ${LOTUS_TEST_BIN_DIR}/type_qualifier_test)
  set_tests_properties(type_qualifier_test PROPERTIES
      TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
endif()
