llvm_map_components_to_libnames(DATALOG_LLVM_LIBS Support)

add_executable(lotus-datalog
  DatalogCli.cpp
  JsonFrontend.cpp)

target_link_libraries(lotus-datalog PRIVATE
  LotusDatalog
  ${DATALOG_LLVM_LIBS})

target_include_directories(lotus-datalog PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS lotus-datalog RUNTIME DESTINATION bin)
