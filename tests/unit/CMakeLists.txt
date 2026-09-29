include(${CMAKE_CURRENT_LIST_DIR}/UnitTestHelpers.cmake)

add_subdirectory(Alias)
add_subdirectory(Analysis)
add_subdirectory(Checker)
add_subdirectory(CFL)
add_subdirectory(Concurrency)
add_subdirectory(Dataflow)
add_subdirectory(Fuzzing)
add_subdirectory(IR)
add_subdirectory(Solvers)
add_subdirectory(SymbolicExecution)
add_subdirectory(Utils)
add_subdirectory(Verification)

# Aggregate build entry point for all unit-test executables:
# `cmake --build <dir> --target lotus_unit_tests` builds just the tests.
get_property(_lotus_unit_test_executables GLOBAL PROPERTY
    LOTUS_UNIT_TEST_EXECUTABLES)
if(_lotus_unit_test_executables)
    list(REMOVE_DUPLICATES _lotus_unit_test_executables)
    add_custom_target(lotus_unit_tests
        DEPENDS ${_lotus_unit_test_executables})
endif()
unset(_lotus_unit_test_executables)
