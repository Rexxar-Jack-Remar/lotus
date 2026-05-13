add_library(CheckerCore STATIC
  CheckerTypes.cpp
  CheckerDiagnostic.cpp
  CheckerRegistry.cpp
  CheckerValidator.cpp
  CheckerSpecLoader.cpp
  CheckerDriver.cpp
)

target_link_libraries(CheckerCore
  PUBLIC
    CanaryReport
    CanaryAliasAnalysisWrapper
    #LLVMCore
    LLVMIRReader
    LLVMSupport
)
