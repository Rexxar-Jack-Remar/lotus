# Verifier umbrella CMake for tools under tools/verifier
#
# Exposes toggles for the various verifier toolchains and delegates
# to their existing CMake configurations within this directory.
#
# Available subprojects:
#   - symbolic_abstraction
#   - clam          (LOTUS_ENABLE_CLAM, defined at top-level)
#   - crabber       (LOTUS_ENABLE_CLAM, defined at top-level, requires CRAB)
#   - seahorn       (LOTUS_ENABLE_SEAHORN, defined at top-level)
#   - smack         (LOTUS_ENABLE_SMACK, defined at top-level)

# SymAbsAI
add_subdirectory(symabs-ai)

# Sifa (Symbolic Interpretation with Fluid Abstractions)
add_subdirectory(sifa)

# CLAM tools (clam, clam-diff, clam-pp) (optional)
if(LOTUS_ENABLE_CLAM)
    add_subdirectory(clam)
endif()

# Seahorn tools (optional)
if(LOTUS_ENABLE_SEAHORN)
    add_subdirectory(seahorn)
endif()

# SMACK tools (optional)
if(LOTUS_ENABLE_SMACK)
    add_subdirectory(smack)
endif()
