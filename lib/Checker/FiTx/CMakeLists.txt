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
  ${FiTx_DIR}/Framework_IR/Analyzer.cpp
  ${FiTx_DIR}/Framework_IR/IRGenerator.cpp
  ${FiTx_DIR}/Framework_IR/Utils.cpp
)

set(FiTx_Detector_SOURCES
  ${FiTx_DIR}/Detector/DFUtils.cpp
  ${FiTx_DIR}/Detector/DLUtils.cpp
  ${FiTx_DIR}/Detector/DULUtils.cpp
  ${FiTx_DIR}/Detector/LeakUtils.cpp
  ${FiTx_DIR}/Detector/NullPtr_Detector.cpp
  ${FiTx_DIR}/Detector/RefUtils.cpp
  ${FiTx_DIR}/Detector/UnrefUtils.cpp
  ${FiTx_DIR}/Detector/UAFUtils.cpp
  ${FiTx_DIR}/Detector/UBI_Detector.cpp
)

add_library(FiTxChecker STATIC
  ${FiTx_Framework_SOURCES}
  ${FiTx_Detector_SOURCES}
)
