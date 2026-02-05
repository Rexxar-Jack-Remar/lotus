# FiTx Checker Library

set(FiTx_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(FiTx_Framework_SOURCES
  ${FiTx_DIR}/core/AnalysisHelper.cpp
  ${FiTx_DIR}/core/BasicBlock.cpp
  ${FiTx_DIR}/core/Function.cpp
  ${FiTx_DIR}/core/Instruction.cpp
  ${FiTx_DIR}/core/Instructions/BranchInstruction.cpp
  ${FiTx_DIR}/core/Instructions/CallInstruction.cpp
  ${FiTx_DIR}/core/Instructions/LoadInstruction.cpp
  ${FiTx_DIR}/core/Instructions/StoreInstruction.cpp
  ${FiTx_DIR}/core/Logs.cpp
  ${FiTx_DIR}/core/SFG/Converter.cpp
  ${FiTx_DIR}/core/Utils.cpp
  ${FiTx_DIR}/core/Value.cpp
  ${FiTx_DIR}/core/ValueTypeAlias.cpp
  ${FiTx_DIR}/frontend/Analyzer.cpp
  ${FiTx_DIR}/frontend/BasicBlock.cpp
  ${FiTx_DIR}/frontend/Framework.cpp
  ${FiTx_DIR}/frontend/Function.cpp
  ${FiTx_DIR}/frontend/State.cpp
  ${FiTx_DIR}/frontend/StateTransition.cpp
  ${FiTx_DIR}/framework_ir/Analyzer.cpp
  ${FiTx_DIR}/framework_ir/IRGenerator.cpp
  ${FiTx_DIR}/framework_ir/Utils.cpp
)

set(FiTx_Detector_SOURCES
  ${FiTx_DIR}/detector/all_detector/All_Detector.cpp
  ${FiTx_DIR}/detector/df_detector/DF_Detector.cpp
  ${FiTx_DIR}/detector/df_detector/DFUtils.cpp
  ${FiTx_DIR}/detector/double_lock_detector/DL_Detector.cpp
  ${FiTx_DIR}/detector/double_lock_detector/DLUtils.cpp
  ${FiTx_DIR}/detector/double_unlock_detector/DUL_Detector.cpp
  ${FiTx_DIR}/detector/double_unlock_detector/DULUtils.cpp
  ${FiTx_DIR}/detector/leak_detector/Leak_Detector.cpp
  ${FiTx_DIR}/detector/leak_detector/LeakUtils.cpp
  ${FiTx_DIR}/detector/nullptr_detector/NULL_Detector.cpp
  ${FiTx_DIR}/detector/ref_count_detector/Ref_Detector.cpp
  ${FiTx_DIR}/detector/ref_count_detector/RefUtils.cpp
  ${FiTx_DIR}/detector/ref_uncount_detector/Unref_Detector.cpp
  ${FiTx_DIR}/detector/ref_uncount_detector/UnrefUtils.cpp
  ${FiTx_DIR}/detector/two_detector/Two_Detector.cpp
  ${FiTx_DIR}/detector/uaf_detector/UAF_Detector.cpp
  ${FiTx_DIR}/detector/uaf_detector/UAFUtils.cpp
  ${FiTx_DIR}/detector/ubi_detector/UBI_Detector.cpp
)

add_library(FiTxChecker STATIC
  ${FiTx_Framework_SOURCES}
  ${FiTx_Detector_SOURCES}
)
