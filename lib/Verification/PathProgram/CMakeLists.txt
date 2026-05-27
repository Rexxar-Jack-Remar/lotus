add_library(VerificationPathProgram STATIC
  CompressedCfg.cpp
  PathProgram.cpp
  PathProgramBuilder.cpp
)

target_link_libraries(VerificationPathProgram
  LLVMCore
  LLVMSupport
)

install(TARGETS VerificationPathProgram DESTINATION lib)
