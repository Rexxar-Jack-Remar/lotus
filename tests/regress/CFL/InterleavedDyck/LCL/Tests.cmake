if(TARGET lotus-cfl-interleaved-dyck)
    add_test(NAME interleaved_dyck_lcl_cli_crossing
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            lcl --query 0 4 ${CMAKE_CURRENT_LIST_DIR}/crossing.dot)
    set_tests_properties(interleaved_dyck_lcl_cli_crossing PROPERTIES
        PASS_REGULAR_EXPRESSION "query: may-reach")
    add_test(NAME interleaved_dyck_lcl_cli_directed
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            lcl --query 4 0 ${CMAKE_CURRENT_LIST_DIR}/crossing.dot)
    set_tests_properties(interleaved_dyck_lcl_cli_directed PROPERTIES
        PASS_REGULAR_EXPRESSION "query: unreachable")
    add_test(NAME interleaved_dyck_lcl_cli_gray
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            lcl --query 0 3 ${CMAKE_CURRENT_LIST_DIR}/spurious.dot)
    set_tests_properties(interleaved_dyck_lcl_cli_gray PROPERTIES
        PASS_REGULAR_EXPRESSION "query: unreachable")
    add_test(NAME interleaved_dyck_lcl_cli_baseline
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            lcl --baseline --query 0 3 ${CMAKE_CURRENT_LIST_DIR}/spurious.dot)
    set_tests_properties(interleaved_dyck_lcl_cli_baseline PROPERTIES
        PASS_REGULAR_EXPRESSION "query: may-reach")
    add_test(NAME interleaved_dyck_lcl_cli_neutral
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            lcl --query -9 99 ${CMAKE_CURRENT_LIST_DIR}/neutral.dot)
    set_tests_properties(interleaved_dyck_lcl_cli_neutral PROPERTIES
        PASS_REGULAR_EXPRESSION "query: may-reach")
    add_test(NAME interleaved_dyck_lcl_cli_bad_query
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            lcl --query not-a-number 4 ${CMAKE_CURRENT_LIST_DIR}/crossing.dot)
    add_test(NAME interleaved_dyck_lcl_cli_missing_vertex
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            lcl --query 0 999 ${CMAKE_CURRENT_LIST_DIR}/crossing.dot)
    add_test(NAME interleaved_dyck_lcl_cli_limit
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            lcl --max-summaries 1 ${CMAKE_CURRENT_LIST_DIR}/crossing.dot)
    set_tests_properties(
        interleaved_dyck_lcl_cli_bad_query
        interleaved_dyck_lcl_cli_missing_vertex
        interleaved_dyck_lcl_cli_limit
        PROPERTIES WILL_FAIL TRUE)
    set_tests_properties(
        interleaved_dyck_lcl_cli_crossing
        interleaved_dyck_lcl_cli_directed
        interleaved_dyck_lcl_cli_gray
        interleaved_dyck_lcl_cli_baseline
        interleaved_dyck_lcl_cli_neutral
        interleaved_dyck_lcl_cli_bad_query
        interleaved_dyck_lcl_cli_missing_vertex
        interleaved_dyck_lcl_cli_limit
        PROPERTIES LABELS "lotus;integration;cfl;cli" TIMEOUT 30)
endif()
