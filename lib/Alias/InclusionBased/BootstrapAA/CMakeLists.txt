find_package(Threads REQUIRED)

add_library(CanaryBootstrapCore STATIC
  Engine.cpp)

target_link_libraries(CanaryBootstrapCore
  PUBLIC
    Threads::Threads)

add_library(CanaryBootstrapAA STATIC
  BootstrapAA.cpp)

target_link_libraries(CanaryBootstrapAA
  PUBLIC
    CanaryBootstrapCore
    LLVMCore
    LLVMSupport)
