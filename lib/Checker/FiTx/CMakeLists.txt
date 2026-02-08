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
  ${FiTx_DIR}/Detector/All_detector/All_Detector.cpp
  ${FiTx_DIR}/Detector/DF_detector/DF_Detector.cpp
  ${FiTx_DIR}/Detector/DF_detector/DFUtils.cpp
  ${FiTx_DIR}/Detector/Double_lock_detector/DL_Detector.cpp
  ${FiTx_DIR}/Detector/Double_lock_detector/DLUtils.cpp
  ${FiTx_DIR}/Detector/Double_unlock_detector/DUL_Detector.cpp
  ${FiTx_DIR}/Detector/Double_unlock_detector/DULUtils.cpp
  ${FiTx_DIR}/Detector/Leak_detector/Leak_Detector.cpp
  ${FiTx_DIR}/Detector/Leak_detector/LeakUtils.cpp
  ${FiTx_DIR}/Detector/NullPtr_detector/NullPtr_Detector.cpp
  ${FiTx_DIR}/Detector/Ref_count_detector/Ref_Detector.cpp
  ${FiTx_DIR}/Detector/Ref_count_detector/RefUtils.cpp
  ${FiTx_DIR}/Detector/Ref_uncount_detector/Unref_Detector.cpp
  ${FiTx_DIR}/Detector/Ref_uncount_detector/UnrefUtils.cpp
  ${FiTx_DIR}/Detector/Two_detector/Two_Detector.cpp
  ${FiTx_DIR}/Detector/UAF_detector/UAF_Detector.cpp
  ${FiTx_DIR}/Detector/UAF_detector/UAFUtils.cpp
  ${FiTx_DIR}/Detector/UBI_detector/UBI_Detector.cpp
)

add_library(FiTxChecker STATIC
  ${FiTx_Framework_SOURCES}
  ${FiTx_Detector_SOURCES}
)
