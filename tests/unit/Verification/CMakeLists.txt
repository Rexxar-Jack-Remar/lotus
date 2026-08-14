add_subdirectory(Sifa)

add_lotus_test_suite(verification_component_tests
  SOURCES
    FailureDirectedTrimmingTest.cpp
    PathProgramTest.cpp
    BooleanProgramFrontendTest.cpp
    DriverTest.cpp
  LINK_LIBS
    FailureDirectedTrimming
    VerificationPathProgram
    VerificationFrontend
    VerificationDriver
    CanaryAliasAnalysisWrapper
    ${Z3_LIBRARIES})

add_custom_target(verification_tests
  DEPENDS
    verification_sifa_tests
    verification_component_tests)
