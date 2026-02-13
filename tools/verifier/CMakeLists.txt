# Verifier umbrella CMake for tools under tools/verifier
#
# Exposes toggles for the various verifier toolchains and delegates
# to their existing CMake configurations within this directory.
#
# Available subprojects:
#   - symbolic_abstraction
#   - clam          (ENABLE_CLAM, defined at top-level)
#   - crabber       (ENABLE_CLAM, defined at top-level, requires CRAB)
#   - horn-ice      (BUILD_HORN_ICE)
#   - seahorn       (ENABLE_SEAHORN, defined at top-level)

# SymbolicAbstraction
add_subdirectory(symbolic-abstraction)

add_executable(lotus-verify lotus-verify.cpp)
target_link_libraries(lotus-verify PRIVATE
  VerificationBackend
  LLVMSupport
)

# Sifa (Symbolic Interpretation with Fluid Abstractions)
add_subdirectory(sifa)

# CLAM tools (clam, clam-diff, clam-pp) (optional)
if(ENABLE_CLAM)
    add_subdirectory(clam)
endif()

# Seahorn tools (optional)
if(ENABLE_SEAHORN)
    add_subdirectory(seahorn)
endif()

# horn-ice
option(BUILD_HORN_ICE "Build ICE learning for CHC and Boogie" OFF)
if(BUILD_HORN_ICE)
    add_subdirectory(horn-ice)
endif()

