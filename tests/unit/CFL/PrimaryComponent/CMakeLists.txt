# These imported tests provide their own mains, an independent grammar oracle,
# and (in the allocation test) global new overrides. Keep them independent of
# GTest/LLVM support libraries to preserve their standalone failure checks.
function(add_primary_component_executable name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE CanaryDynamicDyck)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endfunction()

add_primary_component_executable(dynamic_dyck_primary_component_test PrimaryComponentTests.cpp)
foreach(suite regressions api exhaustive random connectivity dense locality scaling)
    add_test(NAME dynamic_dyck_primary_component_${suite}
        COMMAND dynamic_dyck_primary_component_test ${suite})
    set_tests_properties(dynamic_dyck_primary_component_${suite} PROPERTIES TIMEOUT 240)
    if(NOT suite STREQUAL "connectivity")
        add_test(NAME dynamic_dyck_primary_component_hdt_${suite}
            COMMAND dynamic_dyck_primary_component_test ${suite} 24 1200 hdt)
        set_tests_properties(dynamic_dyck_primary_component_hdt_${suite} PROPERTIES TIMEOUT 240)
    endif()
endforeach()

add_primary_component_executable(dynamic_dyck_primary_component_allocation_failure
    PrimaryComponentAllocationFailure.cpp)
add_test(NAME dynamic_dyck_primary_component_allocation_failure
    COMMAND dynamic_dyck_primary_component_allocation_failure)
add_test(NAME dynamic_dyck_primary_component_hdt_allocation_failure
    COMMAND dynamic_dyck_primary_component_allocation_failure hdt)

add_primary_component_executable(dynamic_dyck_primary_component_sparsification
    PrimaryComponentSparsificationTests.cpp)
add_test(NAME dynamic_dyck_primary_component_sparsification
    COMMAND dynamic_dyck_primary_component_sparsification)
set_tests_properties(dynamic_dyck_primary_component_sparsification PROPERTIES TIMEOUT 240)

# Compile checkpoint hooks only into this isolated test library. Never link it
# and CanaryDynamicDyck into the same executable (duplicate solver definitions).
add_library(dynamic_dyck_primary_component_instrumented STATIC
    ${PROJECT_SOURCE_DIR}/lib/CFL/DynamicDyck/PrimaryComponent/PrimaryComponentSolver.cpp
    ${PROJECT_SOURCE_DIR}/lib/CFL/DynamicDyck/PrimaryComponent/PrimaryConnectivity.cpp
    ${PROJECT_SOURCE_DIR}/lib/CFL/DynamicDyck/PrimaryComponent/SparsifiedConnectivity.cpp)
target_include_directories(dynamic_dyck_primary_component_instrumented PUBLIC
    ${PROJECT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(dynamic_dyck_primary_component_instrumented PRIVATE
    LOTUS_PRIMARY_COMPONENT_TEST_HOOKS=1)
add_executable(dynamic_dyck_primary_component_phases PrimaryComponentPhases.cpp)
target_link_libraries(dynamic_dyck_primary_component_phases PRIVATE
    dynamic_dyck_primary_component_instrumented)
set_target_properties(dynamic_dyck_primary_component_phases PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
add_test(NAME dynamic_dyck_primary_component_phase_invariants
    COMMAND dynamic_dyck_primary_component_phases)
set_tests_properties(dynamic_dyck_primary_component_phase_invariants PROPERTIES TIMEOUT 240)
