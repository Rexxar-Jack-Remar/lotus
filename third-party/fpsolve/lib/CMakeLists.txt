file(GLOB_RECURSE FPSOLVE_PUBLIC_HEADERS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/fpsolve/*.h")
file(GLOB_RECURSE FPSOLVE_LIB_SOURCES CONFIGURE_DEPENDS "*.cpp")

if(NOT USE_GENEPI)
  list(FILTER FPSOLVE_PUBLIC_HEADERS EXCLUDE REGEX ".*/semilinSetNdd.*\\.h$")
  list(FILTER FPSOLVE_LIB_SOURCES EXCLUDE REGEX ".*/semilinSetNdd.*\\.cpp$")
endif()

if(NOT USE_LIBFA)
  list(FILTER FPSOLVE_PUBLIC_HEADERS EXCLUDE REGEX ".*/lossy.*\\.h$")
  list(FILTER FPSOLVE_LIB_SOURCES EXCLUDE REGEX ".*/lossy.*\\.cpp$")
endif()

add_library(fpsolve_lib ${FPSOLVE_PUBLIC_HEADERS} ${FPSOLVE_LIB_SOURCES})
add_library(fpsolve::lib ALIAS fpsolve_lib)

set_source_files_properties(
  "${CMAKE_CURRENT_SOURCE_DIR}/fpsolve/parser.cpp"
  PROPERTIES COMPILE_OPTIONS "-g0")

target_compile_features(fpsolve_lib PUBLIC cxx_std_14)
target_compile_options(fpsolve_lib PRIVATE -O1 -g -pipe)
target_compile_definitions(
  fpsolve_lib
  PUBLIC
    $<$<BOOL:${USE_GENEPI}>:USE_GENEPI>
    $<$<BOOL:${USE_LIBFA}>:USE_LIBFA>)

target_include_directories(
  fpsolve_lib
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    ${Boost_INCLUDE_DIRS}
    ${GMP_INCLUDES})

target_link_libraries(
  fpsolve_lib
  PUBLIC
    ${Boost_LIBRARIES}
    ${GMP_LIBRARIES})

if(USE_GENEPI)
  target_include_directories(fpsolve_lib PUBLIC ${GENEPI_INCLUDE_DIRS})
  target_link_directories(fpsolve_lib PUBLIC ${GENEPI_LIBRARY_DIRS})
  target_link_libraries(fpsolve_lib PUBLIC ${GENEPI_LIBRARIES})
endif()

if(USE_LIBFA)
  target_include_directories(fpsolve_lib PUBLIC ${LIBFA_INCLUDE_DIRS})
  target_link_directories(fpsolve_lib PUBLIC ${LIBFA_LIBRARY_DIRS})
  target_link_libraries(fpsolve_lib PUBLIC ${LIBFA_LIBRARIES})
endif()
