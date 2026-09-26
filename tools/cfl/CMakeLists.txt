# CFL tools umbrella

add_subdirectory(classical)
add_subdirectory(dynamic-dyck)

if(LOTUS_ENABLE_CSR)
    add_subdirectory(csr)
endif()

add_subdirectory(interleaved-dyck)
