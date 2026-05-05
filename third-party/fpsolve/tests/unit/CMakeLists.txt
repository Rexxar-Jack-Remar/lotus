file(GLOB FPSOLVE_TEST_HEADERS CONFIGURE_DEPENDS "*.h")
file(GLOB FPSOLVE_TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

if(NOT USE_GENEPI)
  list(FILTER FPSOLVE_TEST_HEADERS EXCLUDE REGEX ".*/test-semilinSetNdd\\.h$")
  list(FILTER FPSOLVE_TEST_SOURCES EXCLUDE REGEX ".*/test-semilinSetNdd\\.cpp$")
endif()

if(NOT USE_LIBFA)
  list(FILTER FPSOLVE_TEST_HEADERS EXCLUDE REGEX ".*/test-lossy\\.h$")
  list(FILTER FPSOLVE_TEST_SOURCES EXCLUDE REGEX ".*/test-lossy\\.cpp$")
endif()

add_executable(fpsolve_test ${FPSOLVE_TEST_HEADERS} ${FPSOLVE_TEST_SOURCES})
target_compile_features(fpsolve_test PRIVATE cxx_std_14)
target_include_directories(fpsolve_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fpsolve_test PRIVATE fpsolve::lib GTest::gtest)
