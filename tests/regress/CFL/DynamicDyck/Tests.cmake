if(TARGET lotus-cfl-dynamic-dyck)
    foreach(backend deterministic hdt)
        foreach(case initial opaque cyclic)
            if(case STREQUAL "initial")
                set(sequence updates.seq)
            else()
                set(sequence ${case}.seq)
            endif()
            add_test(NAME dynamic_dyck_cli_primary_component_${backend}_${case}
                COMMAND ${CMAKE_COMMAND}
                    -DBINARY=$<TARGET_FILE:lotus-cfl-dynamic-dyck> -DBACKEND=${backend}
                    -DINITIAL=${CMAKE_CURRENT_LIST_DIR}/${case}.dot
                    -DSEQUENCE=${CMAKE_CURRENT_LIST_DIR}/${sequence}
                    -P ${CMAKE_CURRENT_LIST_DIR}/CompareAlgorithms.cmake)
        endforeach()
    endforeach()
    add_test(NAME dynamic_dyck_cli_primary_component_counted
        COMMAND $<TARGET_FILE:lotus-cfl-dynamic-dyck>
            --algorithm primary-component --counted --stats
            ${CMAKE_CURRENT_LIST_DIR}/initial.dot ${CMAKE_CURRENT_LIST_DIR}/updates.seq)
    set_tests_properties(dynamic_dyck_cli_primary_component_counted PROPERTIES
        PASS_REGULAR_EXPRESSION
            "vertices=5 edges=3 components=4 updates=4 changed_updates=4")
    add_test(NAME dynamic_dyck_cli_primary_component_legacy_output
        COMMAND ${CMAKE_COMMAND}
            -DBINARY=$<TARGET_FILE:lotus-cfl-dynamic-dyck> -DMODE=1 -DALGORITHM=primary-component
            -DINITIAL=${CMAKE_CURRENT_LIST_DIR}/initial.dot
            -DSEQUENCE=${CMAKE_CURRENT_LIST_DIR}/updates.seq
            -P ${CMAKE_CURRENT_LIST_DIR}/RunCLI.cmake)
    add_test(NAME dynamic_dyck_cli_primary_component_rejects_recompute
        COMMAND $<TARGET_FILE:lotus-cfl-dynamic-dyck> --algorithm primary-component --recompute
            ${CMAKE_CURRENT_LIST_DIR}/initial.dot ${CMAKE_CURRENT_LIST_DIR}/updates.seq)
    add_test(NAME dynamic_dyck_cli_primary_component_rejects_invalid_label
        COMMAND $<TARGET_FILE:lotus-cfl-dynamic-dyck> --algorithm primary-component
            ${CMAKE_CURRENT_LIST_DIR}/initial.dot ${CMAKE_CURRENT_LIST_DIR}/invalid.seq)
    foreach(option algorithm backend)
        add_test(NAME dynamic_dyck_cli_primary_component_rejects_missing_${option}
            COMMAND $<TARGET_FILE:lotus-cfl-dynamic-dyck> --algorithm primary-component --${option})
        add_test(NAME dynamic_dyck_cli_primary_component_rejects_invalid_${option}
            COMMAND $<TARGET_FILE:lotus-cfl-dynamic-dyck> --algorithm primary-component --${option} unknown
                ${CMAKE_CURRENT_LIST_DIR}/initial.dot ${CMAKE_CURRENT_LIST_DIR}/updates.seq)
        set_tests_properties(dynamic_dyck_cli_primary_component_rejects_missing_${option}
            dynamic_dyck_cli_primary_component_rejects_invalid_${option} PROPERTIES WILL_FAIL TRUE)
    endforeach()
    add_test(NAME dynamic_dyck_cli_primary_component_rejects_counted_without_selection
        COMMAND $<TARGET_FILE:lotus-cfl-dynamic-dyck> --counted
            ${CMAKE_CURRENT_LIST_DIR}/initial.dot ${CMAKE_CURRENT_LIST_DIR}/updates.seq)
    set_tests_properties(dynamic_dyck_cli_primary_component_rejects_recompute
        dynamic_dyck_cli_primary_component_rejects_invalid_label
        dynamic_dyck_cli_primary_component_rejects_counted_without_selection PROPERTIES WILL_FAIL TRUE)
    foreach(mode 0 1)
        add_test(NAME dynamic_dyck_cli_${mode}
            COMMAND $<TARGET_FILE:lotus-cfl-dynamic-dyck> --stats ${mode}
                ${CMAKE_CURRENT_LIST_DIR}/initial.dot
                ${CMAKE_CURRENT_LIST_DIR}/updates.seq)
        set_tests_properties(dynamic_dyck_cli_${mode} PROPERTIES
            PASS_REGULAR_EXPRESSION
                "vertices=5 edges=3 components=4 updates=4 changed_updates=3")

        add_test(NAME dynamic_dyck_cli_opaque_${mode}
            COMMAND $<TARGET_FILE:lotus-cfl-dynamic-dyck> --stats ${mode}
                ${CMAKE_CURRENT_LIST_DIR}/opaque.dot ${CMAKE_CURRENT_LIST_DIR}/opaque.seq)
        set_tests_properties(dynamic_dyck_cli_opaque_${mode} PROPERTIES
            PASS_REGULAR_EXPRESSION
                "vertices=4 edges=2 components=3 updates=4 changed_updates=2")

        add_test(NAME dynamic_dyck_cli_cyclic_${mode}
            COMMAND $<TARGET_FILE:lotus-cfl-dynamic-dyck> --stats ${mode}
                ${CMAKE_CURRENT_LIST_DIR}/cyclic.dot ${CMAKE_CURRENT_LIST_DIR}/cyclic.seq)
        set_tests_properties(dynamic_dyck_cli_cyclic_${mode} PROPERTIES
            PASS_REGULAR_EXPRESSION
                "vertices=5 edges=5 components=5 updates=1 changed_updates=1")

        add_test(NAME dynamic_dyck_cli_legacy_output_${mode}
            COMMAND ${CMAKE_COMMAND}
                -DBINARY=$<TARGET_FILE:lotus-cfl-dynamic-dyck>
                -DMODE=${mode}
                -DINITIAL=${CMAKE_CURRENT_LIST_DIR}/initial.dot
                -DSEQUENCE=${CMAKE_CURRENT_LIST_DIR}/updates.seq
                -P ${CMAKE_CURRENT_LIST_DIR}/RunCLI.cmake)
    endforeach()
    add_test(NAME dynamic_dyck_cli_rejects_invalid_label
        COMMAND $<TARGET_FILE:lotus-cfl-dynamic-dyck>
            ${CMAKE_CURRENT_LIST_DIR}/initial.dot
            ${CMAKE_CURRENT_LIST_DIR}/invalid.seq)
    set_tests_properties(dynamic_dyck_cli_rejects_invalid_label PROPERTIES WILL_FAIL TRUE)
endif()
