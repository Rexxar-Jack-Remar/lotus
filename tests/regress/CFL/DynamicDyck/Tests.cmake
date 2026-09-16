if(TARGET lotus-cfl-dynamic-dyck)
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
