add_library(CanaryBootstrapCore STATIC
  Engine.cpp)

add_library(CanaryBootstrapAA STATIC
  BootstrapAA.cpp)

target_link_libraries(CanaryBootstrapAA
  PUBLIC
    CanaryBootstrapCore
    LLVMCore
    LLVMSupport)
