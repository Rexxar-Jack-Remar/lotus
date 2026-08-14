# Utility Unit Tests

add_subdirectory(Parallel)

add_lotus_test_suite(utils_general_tests
  SOURCES
    GraphSlicerTest.cpp
    LLVMCallUtilsTest.cpp
    LLVMFunctionUtilsTest.cpp
    StringUtilsTest.cpp
    PathExpressionsTest.cpp
  LINK_LIBS
    CanaryADT
    CanaryLLVMUtils
    CanaryFormats)

set(LOTUS_UTILS_TEST_TARGETS
    utils_general_tests
    utils_parallel_tests
    thread_pool_harness_test
    parallel_tabulation_harness_test
    pipeline_scheduler_harness_test)

if(TARGET Crab)
    add_lotus_targeted_test(crab_machine_int_domain_test CrabMachineIntDomainTest.cpp
        LINK_LIBS
            Crab
        INCLUDE_DIRS
            ${CMAKE_SOURCE_DIR}/third-party/crab/include)
    list(APPEND LOTUS_UTILS_TEST_TARGETS crab_machine_int_domain_test)
else()
    message(STATUS "Skipping crab_machine_int_domain_test: Crab target is not available")
endif()

add_custom_target(utils_tests DEPENDS ${LOTUS_UTILS_TEST_TARGETS})
