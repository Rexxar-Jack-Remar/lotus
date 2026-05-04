add_library(CanaryLIF STATIC
  Taint.cpp
  CCFG.cpp
  Cond.cpp
  Func.cpp
  Isochronous.cpp
  Loop.cpp
)

target_link_libraries(CanaryLIF
  #LLVMAnalysis
  #LLVMCore
  #LLVMPasses
  LLVMTransformUtils
  #LLVMSupport
)
