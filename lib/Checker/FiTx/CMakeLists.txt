# FiTx Checker Library

set(FiTx_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(FiTx_Framework_SOURCES
  ${FiTx_DIR}/Core/AnalysisHelper.cpp
  ${FiTx_DIR}/Core/BasicBlock.cpp
  ${FiTx_DIR}/Core/Function.cpp
  ${FiTx_DIR}/Core/Instruction.cpp
  ${FiTx_DIR}/Core/Instructions/BranchInstruction.cpp
  ${FiTx_DIR}/Core/Instructions/CallInstruction.cpp
  ${FiTx_DIR}/Core/Instructions/LoadInstruction.cpp
  ${FiTx_DIR}/Core/Instructions/StoreInstruction.cpp
  ${FiTx_DIR}/Core/Logs.cpp
  ${FiTx_DIR}/Core/SFG/Converter.cpp
  ${FiTx_DIR}/Core/Utils.cpp
  ${FiTx_DIR}/Core/Value.cpp
  ${FiTx_DIR}/Core/ValueTypeAlias.cpp
  ${FiTx_DIR}/Frontend/Analyzer.cpp
  ${FiTx_DIR}/Frontend/BasicBlock.cpp
  ${FiTx_DIR}/Frontend/Framework.cpp
  ${FiTx_DIR}/Frontend/Function.cpp
  ${FiTx_DIR}/Frontend/State.cpp
  ${FiTx_DIR}/Frontend/StateTransition.cpp
  ${FiTx_DIR}/FrameworkIR/Analyzer.cpp
  ${FiTx_DIR}/FrameworkIR/IRGenerator.cpp
  ${FiTx_DIR}/FrameworkIR/Utils.cpp
)

set(FiTx_Detector_SOURCES
  ${FiTx_DIR}/Detector/DoubleFreeDetector.cpp
  ${FiTx_DIR}/Detector/DoubleLockDetector.cpp
  ${FiTx_DIR}/Detector/DoubleUnlockDetector.cpp
  ${FiTx_DIR}/Detector/MemoryLeakDetector.cpp
  ${FiTx_DIR}/Detector/NullPointerDetector.cpp
  ${FiTx_DIR}/Detector/ReferenceCounterDetector.cpp
  ${FiTx_DIR}/Detector/UnreferenceCounterDetector.cpp
  ${FiTx_DIR}/Detector/UseAfterFreeDetector.cpp
  ${FiTx_DIR}/Detector/UseBeforeInitDetector.cpp
)

add_library(FiTxChecker STATIC
  ${FiTx_Framework_SOURCES}
  ${FiTx_Detector_SOURCES}
)
