add_executable(lotus-solver-datalog lotus-solver-datalog.cpp)

target_link_libraries(lotus-solver-datalog PRIVATE
  LotusDatalogFrontend)

install(TARGETS lotus-solver-datalog RUNTIME DESTINATION bin)
