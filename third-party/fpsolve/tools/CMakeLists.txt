add_executable(${PROJECT_NAME} fpsolve_main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE fpsolve::lib)

if(USE_GENEPI AND USE_LIBFA)
  add_executable(gr_check gr_check.cpp)
  target_link_libraries(gr_check PRIVATE fpsolve::lib)
endif()
