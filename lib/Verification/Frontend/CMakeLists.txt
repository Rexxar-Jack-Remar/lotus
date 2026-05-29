add_library(VerificationFrontend STATIC
  BooleanProgram/BooleanProgram.cpp
  BooleanProgram/BooleanProgramParser.cpp
  BooleanProgram/PredicateProgramLowering.cpp
)

target_link_libraries(VerificationFrontend
  PUBLIC
  NPADataFlow
  LLVMSupport
)
