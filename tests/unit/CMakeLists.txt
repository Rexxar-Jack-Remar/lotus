# Unit Tests CMakeLists.txt for Lotus

cmake_minimum_required(VERSION 3.10)

# Define paths
set(LOTUS_BUILD_DIR ${CMAKE_BINARY_DIR})
set(LOTUS_SRC_DIR ${CMAKE_SOURCE_DIR})

# Add TestUtils include directory
include_directories(TestUtils)

# Add subdirectories
add_subdirectory(Checker)
add_subdirectory(Analysis)
add_subdirectory(Concurrency)
add_subdirectory(ControlFlow)
add_subdirectory(DataFlow)
add_subdirectory(IR)
add_subdirectory(Pulse)
add_subdirectory(Pointer)
add_subdirectory(TypeHierarchy)
add_subdirectory(Verification)
add_subdirectory(Utils)
