cmake_minimum_required(VERSION 3.10)
project(lotus C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set C++14 standard and compiler flags
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -frtti")
# -fno-rtti #we need RTTI.

# Set output directories
# Libraries and archives go to lib/
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
# Default runtime output (tools) goes to bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Test binaries go to bin/tests/
set(LOTUS_TEST_BIN_DIR ${CMAKE_BINARY_DIR}/bin/tests CACHE PATH "Directory for test binaries")
# Example binaries go to bin/examples/
set(LOTUS_EXAMPLE_BIN_DIR ${CMAKE_BINARY_DIR}/bin/examples CACHE PATH "Directory for example binaries")

# Find and configure LLVM (factored out for clarity)
include(cmake/ConfigureLLVM.cmake)

# Include dependency configuration modules
include(cmake/FindZ3.cmake)
include(cmake/ConfigureBoost.cmake)

option(ENABLE_CLAM "Enable CLAM abstract interpretation framework" ON)
option(DOWNLOAD_CRAB "Download and build CRAB if not found" ON)
set(CUSTOM_CRAB_ROOT "" CACHE PATH "Path to custom CRAB installation (optional)")
if (ENABLE_CLAM)
    include(cmake/ConfigureClamCrab.cmake)
endif()

option(ENABLE_SVF "Enable SVF" OFF)
if (ENABLE_SVF)
    include(cmake/ConfigureSVF.cmake)
endif()

# Do not build Seahorn by default
option(ENABLE_SEAHORN "Enable SeaHorn" ON)
if (ENABLE_SEAHORN)
    include(cmake/ConfigureSeaHorn.cmake)
endif()

# Optional cclyzer++ Datalog-based alias analysis (standalone backend, not in AliasAnalysisWrapper)
option(LOTUS_USE_CCLYZER "Enable optional cclyzer++ alias analysis backend" OFF)
if (LOTUS_USE_CCLYZER)
    include(cmake/ConfigureCclyzerpp.cmake)
endif()

# Add subdirectories
add_subdirectory(lib)
add_subdirectory(tools)
add_subdirectory(benchmarks)

option(BUILD_EXAMPLES "Build examples" OFF)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Add tests if enabled
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    include(cmake/FindGTest.cmake)
    enable_testing()
    add_subdirectory(tests)
endif()

# Add Python scripts and utilities
add_subdirectory(scripts)
