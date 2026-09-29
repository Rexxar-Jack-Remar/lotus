# Primary-component refinement solver and its undirected-connectivity backends.
target_sources(CanaryDynamicDyck PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/PrimaryComponentSolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PrimaryConnectivity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SparsifiedConnectivity.cpp)

if(LOTUS_BUILD_TESTS)
    # Phase-invariant tests share cfl_tests with the other CFL cases. Enable
    # their inspection hooks in the library used by that single executable.
    target_compile_definitions(CanaryDynamicDyck PRIVATE
        LOTUS_PRIMARY_COMPONENT_TEST_HOOKS=1)
    target_include_directories(CanaryDynamicDyck PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/unit/CFL/DynamicDyck/PrimaryComponent)
endif()
