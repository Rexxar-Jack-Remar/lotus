add_library(VerificationFrontend STATIC
  BooleanProgram.cpp
  BooleanProgramParser.cpp
  PredicateProgramLowering.cpp
)

target_link_libraries(VerificationFrontend
  PUBLIC
  NPADataFlow
  LLVMSupport
)
