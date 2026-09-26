add_library(DemandAPA INTERFACE)
target_include_directories(DemandAPA INTERFACE
  ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(DemandAPA INTERFACE lotus_buddy)
find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
  target_link_libraries(DemandAPA INTERFACE OpenMP::OpenMP_CXX)
endif()
