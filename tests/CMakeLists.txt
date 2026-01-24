# TODO: Following the code of clam to add the tests in tests/clam

cmake_minimum_required(VERSION 3.10)
find_package(LLVM REQUIRED CONFIG)
# GTest should already be found or downloaded by FindGTest.cmake in parent CMakeLists.txt
# Try find_package, but if targets already exist (from add_subdirectory), that's fine too
find_package(GTest QUIET)
if(NOT TARGET GTest::gtest AND NOT TARGET gtest)
    message(FATAL_ERROR "GTest not found. This should have been handled by FindGTest.cmake")
endif()

add_definitions(${LLVM_DEFINITIONS})
add_definitions(-DCMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

set(COMMON_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/utils ${CMAKE_CURRENT_SOURCE_DIR}/../include ${LLVM_INCLUDE_DIRS})
set(COMMON_LIBS ${LLVM_LIBRARIES} GTest::gtest GTest::gtest_main IFDS Annotation CanaryAliasAnalysisWrapper CanaryDyckAA LLVMIRReader CanaryPDG Concurrency SymAbs CanaryICFG AserPTA MONODataFlow WPDSDataFlow wpds++ CanaryTypeHirarchy CanaryTransform FailureDirectedTrimming)

add_library(lotus_test_utils INTERFACE)
target_include_directories(lotus_test_utils INTERFACE ${COMMON_INCLUDES})
target_link_libraries(lotus_test_utils INTERFACE ${COMMON_LIBS})

function(add_lotus_test test_name source_file)
    # Check if source_file is a full path or relative
    if(IS_ABSOLUTE ${source_file})
        set(source ${source_file})
    else()
        set(source ${CMAKE_CURRENT_SOURCE_DIR}/${source_file})
    endif()
    
    add_executable(${test_name} ${source})
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_include_directories(${test_name} PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(${test_name} lotus_test_utils)
    
    # Register with CTest
    add_test(NAME ${test_name} COMMAND ${LOTUS_TEST_BIN_DIR}/${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 300 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endfunction()

function(add_lotus_pdg_test test_name source_file)
    # Check if source_file is a full path or relative
    if(IS_ABSOLUTE ${source_file})
        set(source ${source_file})
    else()
        set(source ${CMAKE_CURRENT_SOURCE_DIR}/${source_file})
    endif()
    
    add_executable(${test_name} ${source})
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_include_directories(${test_name} PRIVATE ${COMMON_INCLUDES})
    # PDG tests need additional LLVM analysis libraries
    target_link_libraries(${test_name} lotus_test_utils LLVMTransformUtils)
    
    # Register with CTest
    add_test(NAME ${test_name} COMMAND ${LOTUS_TEST_BIN_DIR}/${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 300 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endfunction()

# Add unit test subdirectory
add_subdirectory(unit)

# Note: Individual unit tests are now registered in their respective subdirectories under unit/
# The tests are automatically registered using add_lotus_test() and add_lotus_pdg_test() functions

enable_testing()

# Custom target to run all tests
add_custom_target(run_all_tests 
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Custom target to run only unit tests
add_custom_target(run_unit_tests 
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
