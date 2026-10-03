# VASCO DataFlow Unit Tests

lotus_collect_test_sources(dataflow_tests
  VASCOTest.cpp
  VASCOLLVMClientTest.cpp
  VASCOAdditionalLLVMClientTest.cpp)
lotus_collect_test_sources(dataflow_tests VASCOParallelHarnessTest.cpp)
