# Optional CclyzerAA wrapper (cclyzer++ Datalog-based alias analysis).
# Built only when LOTUS_USE_CCLYZER=ON and cclyzerpp was configured successfully.

add_library(LotusCclyzerAA STATIC CclyzerAA.cpp)

target_include_directories(LotusCclyzerAA
  PUBLIC ${CMAKE_SOURCE_DIR}/include
  PRIVATE ${CMAKE_SOURCE_DIR}/include/Alias/CclyzerAA)

target_compile_features(LotusCclyzerAA PUBLIC cxx_std_17)
target_link_libraries(LotusCclyzerAA PRIVATE PAPass)

if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(LotusCclyzerAA PRIVATE -fno-rtti)
endif()
