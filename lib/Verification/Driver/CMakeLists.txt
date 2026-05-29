add_library(VerificationDriver STATIC
  Backend.cpp
)

target_link_libraries(VerificationDriver
  PUBLIC
  LLVMSupport
)
