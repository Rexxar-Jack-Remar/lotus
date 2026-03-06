# IFDS/IDE Unit Tests

# IFDS Framework Core Test (migrated from phasar-development)
add_lotus_test(ifds_framework_core_test IFDSFrameworkCoreTest.cpp)

# IFDS Solver Test
add_lotus_test(ifds_solver_test IFDSSolverTest.cpp)

# Taint Analysis Test
add_lotus_test(taint_analysis_test TaintAnalysisTest.cpp)

# IDE EdgeFunction / composition tests (ported from phasar-development)
add_lotus_test(ide_edge_function_test IDEEdgeFunctionTest.cpp)

# IDE Constant Propagation solver test (ported from phasar-development)
add_lotus_test(ide_constant_propagation_test IDEConstantPropagationTest.cpp)

# IDE jump-function join semantics
add_lotus_test(ide_jump_function_join_test IDEJumpFunctionJoinTest.cpp)

# Dedicated tests for newly added IDE client analyses
add_lotus_test(ide_new_analyses_test IDENewAnalysesTest.cpp)

# LLVM flow helper policy tests
add_lotus_test(llvm_flow_helpers_test LLVMFlowHelpersTest.cpp)

# LLVM flow helper mapping tests (callee/caller mappings)
add_lotus_test(llvm_flow_helpers_mapping_test LLVMFlowHelpersMappingTest.cpp)

# IFDS client analyses flow behavior on helper-migrated paths
add_lotus_test(ifds_client_alias_flow_test IFDSClientAliasFlowTest.cpp)

# IFDS solver-level integration for alias-aware client behavior
add_lotus_test(ifds_solver_alias_integration_test IFDSSolverAliasIntegrationTest.cpp)

# IFDS/IDE solver invariants and path-aware behavior
add_lotus_test(ifds_solver_invariants_test IFDSSolverInvariantsTest.cpp)
add_lotus_test(pathaware_solver_test PathAwareSolverTest.cpp)
add_lotus_test(return_site_and_config_test ReturnSiteAndConfigTest.cpp)
