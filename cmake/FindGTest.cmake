# Find and configure Google Test
# If LOTUS_BUILD_TESTS is ON and GTest is not found, download and build it in build/deps

# First, try to find GTest using CMake's built-in find_package
find_package(GTest QUIET)

if(NOT GTest_FOUND AND LOTUS_BUILD_TESTS)
    # GTest not found, download it to build/deps at configure time
    set(GTEST_ROOT "${CMAKE_BINARY_DIR}/deps/googletest")
    
    if(NOT EXISTS "${GTEST_ROOT}/CMakeLists.txt")
        find_package(Git REQUIRED)
        message(STATUS "GTest not found, downloading to ${GTEST_ROOT}...")
        
        # Clone Google Test at configure time
        execute_process(
            COMMAND ${GIT_EXECUTABLE} clone --depth 1 https://github.com/google/googletest.git ${GTEST_ROOT}
            RESULT_VARIABLE GIT_RESULT
            OUTPUT_VARIABLE GIT_OUTPUT
            ERROR_VARIABLE GIT_ERROR
        )
        
        if(NOT GIT_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to clone Google Test: ${GIT_ERROR}")
        endif()
        
        if(NOT EXISTS "${GTEST_ROOT}/CMakeLists.txt")
            message(FATAL_ERROR "Google Test clone succeeded but CMakeLists.txt not found")
        endif()
        
        message(STATUS "Google Test successfully downloaded to ${GTEST_ROOT}")
    else()
        message(STATUS "Using previously downloaded Google Test at: ${GTEST_ROOT}")
    endif()
    
    # Prevent Google Test from adding tests to the test target
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_build_tests OFF CACHE BOOL "" FORCE)
    set(gtest_build_samples OFF CACHE BOOL "" FORCE)
    set(gtest_disable_pthreads OFF CACHE BOOL "" FORCE)
    
    # Add Google Test as subdirectory
    add_subdirectory(${GTEST_ROOT} ${CMAKE_BINARY_DIR}/googletest EXCLUDE_FROM_ALL)
    
    # Set GTest_FOUND to indicate we have GTest available
    set(GTest_FOUND TRUE)
    message(STATUS "Using Google Test from: ${GTEST_ROOT}")
    
elseif(NOT GTest_FOUND)
    message(WARNING 
        "GTest not found and LOTUS_BUILD_TESTS is OFF. Tests will not be built.\n"
        "To enable tests, set -DLOTUS_BUILD_TESTS=ON")
else()
    message(STATUS "Found Google Test: ${GTest_DIR}")
endif()
