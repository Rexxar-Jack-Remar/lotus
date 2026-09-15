add_lotus_test_suite(bootstrap_aa_tests
  SOURCES
    BootstrapAATest.cpp
    EngineTest.cpp
  LINK_LIBS
    CanaryBootstrapAA)
add_dependencies(bootstrap_aa_tests lotus-alias-bootstrap)

add_test(NAME bootstrap_aa_cli_smoke
  COMMAND $<TARGET_FILE:lotus-alias-bootstrap>
    --detailed-stats
    --threads=2
    ${CMAKE_SOURCE_DIR}/tests/regress/Alias/BootstrapAA/basic.ll)
set_tests_properties(bootstrap_aa_cli_smoke PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  LABELS "lotus;integration;alias")
