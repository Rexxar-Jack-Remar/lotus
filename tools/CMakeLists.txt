add_subdirectory(alias)
add_subdirectory(checker)
add_subdirectory(dataflow)
add_subdirectory(optimization)
add_subdirectory(verifier)

add_subdirectory(ir)
#add_subdirectory(mcp)        # not used for now

# not used for now
if(LOTUS_ENABLE_CFL)
    add_subdirectory(cfl)
endif()

# Optional SMT tools
if(LOTUS_ENABLE_OWL)
    add_subdirectory(solver)
endif()
