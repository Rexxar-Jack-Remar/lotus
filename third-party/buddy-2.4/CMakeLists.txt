add_library(lotus_buddy STATIC
  src/bddio.c
  src/bddop.c
  src/bvec.c
  src/cache.c
  src/cppext.cxx
  src/fdd.c
  src/imatrix.c
  src/kernel.c
  src/pairs.c
  src/prime.c
  src/reorder.c
  src/tree.c)
target_include_directories(lotus_buddy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(lotus_buddy PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
configure_file(config.h.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/config.h @ONLY)
