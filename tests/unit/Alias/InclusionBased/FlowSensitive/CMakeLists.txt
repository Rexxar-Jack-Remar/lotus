add_lotus_test_suite(flow_sensitive_pta_tests
  SOURCES
    FlowSensitivePTATest.cpp
    ValueFlowGraphTest.cpp
    ValueFlowPTATest.cpp
    VersionedFlowSensitivePTATest.cpp
  LINK_LIBS
    FlowSensitivePTA)
