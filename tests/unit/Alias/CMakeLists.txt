# Alias Analysis Unit Tests
add_lotus_alloc_aa_test(alloc_aa_test AllocAATest.cpp)
add_lotus_aser_pta_test(aser_pta_test AserPTATest.cpp)
add_lotus_dda_test(dda_aa_test DDAATest.cpp)
target_sources(dda_aa_test PRIVATE DDAATestPart2.cpp)
add_lotus_dyck_aa_test(dyck_aa_test DyckAATest.cpp)
add_lotus_sparrow_test(sparrow_aa_test SparrowAATest.cpp)
add_lotus_tpa_test(tpa_test TPATest.cpp)
add_lotus_underapprox_aa_test(underapprox_aa_test UnderApproxAATest.cpp)
add_lotus_underapprox_aa_test(alias_graph_test AliasGraphTest.cpp)
add_lotus_dfpa_test(dfpa_test DFPATest.cpp)
add_lotus_pointer_test(lotus_aa_test LotusAATest.cpp)
target_sources(lotus_aa_test PRIVATE
  LotusAATestPart2.cpp
  LotusAATestPart3.cpp)
add_lotus_gvfg_test(gvfg_lotus_adapter_test GVFGAdapterTest.cpp)
target_sources(gvfg_lotus_adapter_test PRIVATE GVFGAdapterTestPart2.cpp)
