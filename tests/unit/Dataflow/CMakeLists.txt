# Dataflow Unit Tests

add_subdirectory(IfdsIde)
add_subdirectory(Mono)
add_subdirectory(WPDS)
add_subdirectory(APA)
add_subdirectory(NPA)
add_subdirectory(VASCO)

# All dataflow test sources collected from the subdirectories above are linked
# into one binary so the shared libraries are linked only once.
add_lotus_collected_test_suite(dataflow_tests
  LINK_LIBS
    APADataFlow
    LotusEGraph
    IFDS
    MONODataFlow
    NPADataFlow
    VASCODataFlow
    WPDSDataFlow
    wpds++
    CanaryAliasAnalysisWrapper)
