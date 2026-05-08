# Third-party libraries (CUDD, WPDS, spdlog)
add_subdirectory(CUDD)
add_subdirectory(WPDS)
add_subdirectory(spdlog)
if(ENABLE_FPSOLVE)
  # Vendor FPsolve as a library dependency without pulling in its optional
  # test/tool surface or extra external packages by default.
  set(FPSOLVE_BUILD_TOOLS OFF CACHE BOOL "Build FPsolve command-line tools" FORCE)
  set(FPSOLVE_BUILD_TESTS OFF CACHE BOOL "Build FPsolve unit tests" FORCE)
  set(USE_GENEPI OFF CACHE BOOL "Use genepi and semilinear-set NDD support" FORCE)
  set(USE_LIBFA OFF CACHE BOOL "Use libfa and lossy finite automata support" FORCE)
  add_subdirectory(fpsolve EXCLUDE_FROM_ALL)
endif()
# add_subdirectory(mde)
