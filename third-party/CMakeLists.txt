# Third-party libraries (CUDD, WPDS, spdlog)
add_subdirectory(CUDD)
add_subdirectory(WPDS)
add_subdirectory(spdlog)
if(LOTUS_ENABLE_HORN_ICE)
  add_subdirectory(horn-ice)
endif()
if(LOTUS_ENABLE_WALI_OPENNWA)
  # Keep the larger WALi/OpenNWA distribution opt-in. Lotus already builds its
  # legacy WPDS subset above, so this only exposes the full WALi::wali target
  # when explicitly requested.
  set(WALI_BUILD_SHARED OFF CACHE BOOL "Build WALi libraries as shared libraries" FORCE)
  set(WALI_BUILD_TOOLS OFF CACHE BOOL "Build OpenNWA command-line tools" FORCE)
  set(WALI_BUILD_ADDONS OFF CACHE BOOL "Build optional WALi add-on libraries" FORCE)
  set(WALI_BUILD_EXAMPLES OFF CACHE BOOL "Build WALi example programs" FORCE)
  add_subdirectory(WALi-OpenNWA EXCLUDE_FROM_ALL)
endif()
if(LOTUS_ENABLE_FPSOLVE)
  # Vendor FPsolve as a library dependency without pulling in its optional
  # test/tool surface or extra external packages by default.
  set(FPSOLVE_BUILD_TOOLS OFF CACHE BOOL "Build FPsolve command-line tools" FORCE)
  set(FPSOLVE_BUILD_TESTS OFF CACHE BOOL "Build FPsolve unit tests" FORCE)
  set(USE_GENEPI OFF CACHE BOOL "Use genepi and semilinear-set NDD support" FORCE)
  set(USE_LIBFA OFF CACHE BOOL "Use libfa and lossy finite automata support" FORCE)
  add_subdirectory(fpsolve EXCLUDE_FROM_ALL)
endif()
# add_subdirectory(mde)
