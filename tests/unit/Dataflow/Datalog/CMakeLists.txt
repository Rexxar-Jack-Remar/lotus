add_lotus_test_suite(datalog_tests
  SOURCES
    CoreTest.cpp
    SemanticTest.cpp
    ParallelTest.cpp
    ExtensionTest.cpp
  LINK_LIBS LotusDatalog)
