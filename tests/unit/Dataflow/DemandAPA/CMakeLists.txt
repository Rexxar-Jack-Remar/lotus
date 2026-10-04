# Compile each public header as the first and only include of a separate TU.
# Linking them together also catches header-defined symbols with non-inline
# linkage; unity builds and PCHs must not mask missing includes.
set(demand_apa_headers
  Algebra.h
  Algorithm.h
  ApaInstance.h
  BpReader.h
  CentroidPreprocessing.h
  Comparison.h
  DemandOmp.h
  FunctionSummaries.h
  IfdsReader.h
  Interprocedural.h
  Intraprocedural.h
  LCA.h
  Naive.h
  PrePreprocess.h
  Project.h
  Reader.h
  RegEx.h
  SCC.h
  SPReader.h
  SameBagPreprocessing.h
  SparseTable.h
  Support.h
  Tarjan.h
  TreeDecomposition.h
)
set(header_checks)
foreach(header IN LISTS demand_apa_headers)
  set(source "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cpp")
  file(GENERATE OUTPUT "${source}"
    CONTENT "#include \"Dataflow/DemandAPA/${header}\"\n")
  list(APPEND header_checks "${source}")
endforeach()

add_lotus_test_suite(demand_apa_tests
  SOURCES HeaderIntegrationTest.cpp HeaderOrderPeer.cpp ${header_checks}
  LINK_LIBS DemandAPA
  LABELS demandapa)
set_target_properties(demand_apa_tests PROPERTIES
  UNITY_BUILD OFF
  DISABLE_PRECOMPILE_HEADERS ON)
