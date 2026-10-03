if(TARGET lotus-cfl-interleaved-dyck)
    foreach(case correlation automatic identity independent backward json crossing mismatch prefix
            explicit source target allpairs batch empty isolated observer_error unknown mode_error
            limit dimension_limit automatic_dimension_limit missing_vertex conflicting_options help timings missing_scope roundtrip)
        add_test(NAME affine_spds_cli_${case}
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:lotus-cfl-interleaved-dyck>
                -DFIXTURES=${CMAKE_CURRENT_LIST_DIR}
                -DCASE=${case}
                -DWORK=${CMAKE_CURRENT_BINARY_DIR}
                -P ${CMAKE_CURRENT_LIST_DIR}/RunCLI.cmake)
        set_tests_properties(affine_spds_cli_${case} PROPERTIES TIMEOUT 60)
    endforeach()
endif()
