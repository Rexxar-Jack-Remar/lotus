//===- AEDetector.cpp -- Vulnerability Detectors--------------------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#include "Checker/AE/AEDetector.h"

#include "Checker/AE/AbsExtAPI.h"
#include "Checker/AE/AbstractInterpretation.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/BugTypes.h"

#include <algorithm>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace analysis {

namespace {
int getOrRegisterAEBugType(AEDetector::DetectorKind kind) {
  BugReportMgr &mgr = BugReportMgr::get_instance();
  switch (kind) {
  case AEDetector::BUF_OVERFLOW: {
    int id = mgr.find_bug_type("AE Buffer Overflow");
    if (id < 0) {
      id = mgr.register_bug_type("AE Buffer Overflow", BugDescription::BI_HIGH,
                                 BugDescription::BC_SECURITY, "CWE-120, CWE-122");
    }
    return id;
  }
  case AEDetector::NULL_DEREF: {
    int id = mgr.find_bug_type("AE Null Dereference");
    if (id < 0) {
      id = mgr.register_bug_type("AE Null Dereference", BugDescription::BI_HIGH,
                                 BugDescription::BC_SECURITY, "CWE-476");
    }
    return id;
  }
  case AEDetector::USE_AFTER_FREE: {
    int id = mgr.find_bug_type("AE Use After Free");
    if (id < 0) {
      id = mgr.register_bug_type("AE Use After Free", BugDescription::BI_HIGH,
                                 BugDescription::BC_SECURITY, "CWE-416");
    }
    return id;
  }
  case AEDetector::INVALID_FREE: {
    int id = mgr.find_bug_type("AE Invalid Free");
    if (id < 0) {
      id = mgr.register_bug_type("AE Invalid Free", BugDescription::BI_HIGH,
                                 BugDescription::BC_SECURITY, "CWE-590");
    }
    return id;
  }
  default:
    break;
  }
  return -1;
}

void emitAEBugReport(AEDetector::DetectorKind kind, const llvm::Instruction *inst,
                     const std::string &message) {
  int tyId = getOrRegisterAEBugType(kind);
  if (tyId < 0)
    return;

  BugReport *report = new BugReport(tyId);
  report->append_step(const_cast<llvm::Instruction *>(inst), message);
  BugReportMgr::get_instance().insert_report(tyId, report, true);
}
} // namespace

void AEDetector::addEventToTrace(AEBugEventType type,
                                 const llvm::Instruction *inst,
                                 const std::string &desc) {
  eventTrace.emplace_back(type, inst, desc);
}

void AEDetector::clearEventTrace() { eventTrace.clear(); }

/// @brief Detects buffer overflow issues for a given instruction.
///
/// This function handles GEP (GetElementPtr) instructions to detect potential
/// buffer overflows by comparing the access offset against the object size.
///
/// @param as Reference to the abstract state containing object sizes and
/// addresses.
/// @param inst Pointer to the instruction to analyze (must be a GEP
/// instruction).
void BufOverflowDetector::detect(AbstractState &as,
                                 const llvm::Instruction *inst) {
  // Check for buffer overflow in GEP instructions
  if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(inst)) {
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(gep->getPointerOperand());
    uint32_t lhsId = AbstractInterpretation::getValueIdStatic(gep);

    if (as.inVarToAddrsTable(ptrId)) {
      // Get GEP addresses
      AddressValue gepAddrs = as[lhsId].getAddrs();
      AddressValue objAddrs = as[ptrId].getAddrs();
      IntervalValue offset = as.getByteOffset(gep);

      // Track offset for this GEP first
      IntervalValue accumulatedOffset = offset;

      // Check if the pointer operand is itself a GEP (nested GEP case)
      // If so, accumulate the offset from the previous GEP
      if (const auto *prevGep = llvm::dyn_cast<llvm::GetElementPtrInst>(
              gep->getPointerOperand())) {
        if (hasGepObjOffsetFromBase(prevGep)) {
          IntervalValue prevOffset = getGepObjOffsetFromBase(prevGep);
          accumulatedOffset = prevOffset + offset;
        }
      }

      // Store the accumulated offset for this GEP
      addToGepObjOffsetFromBase(gep, accumulatedOffset);

      // Update GEP offset tracking (for compatibility with existing code)
      updateGepObjOffsetFromBase(as, gepAddrs, objAddrs, accumulatedOffset);

      for (auto addr : as[ptrId].getAddrs()) {
        uint32_t objId = as.getIDFromAddr(addr);
        // Compute access offset per object to preserve field/object sensitivity.
        IntervalValue accessOffset = getAccessOffset(as, objId, gep);
        uint32_t objSize = as.getObjSize(objId);

        if (objSize > 0 && accessOffset.ub().getIntNumeral() >=
                               static_cast<int64_t>(objSize)) {
          AEException bug("Buffer overflow: access offset [" +
                          accessOffset.toString() + "] exceeds object size " +
                          std::to_string(objSize));
          addBugToReporter(bug, inst);
        }
      }
    }
  }

  // Check for buffer overflow in external API calls
  // Use annotation-based classification from AEExtAPI
  if (const auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
    if (const llvm::Function *callee = call->getCalledFunction()) {
      AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
      AEExtAPI *utils = ae.getUtils();
      if (utils) {
        AEExtAPI::ExtAPIType extType = utils->getExtAPIType(callee);
        if (extType == AEExtAPI::MEMCPY || extType == AEExtAPI::MEMSET ||
            extType == AEExtAPI::STRCPY || extType == AEExtAPI::STRCAT) {
          detectExtAPI(as, call);
        }
      } else {
        // Fallback to string matching if utils not available
        std::string funName = callee->getName().str();
        if (funName.find("memcpy") != std::string::npos ||
            funName.find("memmove") != std::string::npos ||
            funName.find("memset") != std::string::npos ||
            funName.find("strcpy") != std::string::npos ||
            funName.find("strcat") != std::string::npos ||
            funName.find("strncpy") != std::string::npos ||
            funName.find("strncat") != std::string::npos) {
          detectExtAPI(as, call);
        }
      }
    }
  }
}

void BufOverflowDetector::handleStubFunctions(const llvm::CallBase *call) {
  if (!call->getCalledFunction())
    return;

  std::string funcName = call->getCalledFunction()->getName().str();

  if (funcName == "SAFE_BUFACCESS") {
    AbstractInterpretation::getAEInstance().markCheckpointChecked(call);
    AbstractInterpretation::getAEInstance().checkpoints.erase(call);
    if (call->arg_size() < 2)
      return;

    AbstractState &as =
        AbstractInterpretation::getAEInstance().getAbsStateFromTrace(call);

    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));
    uint32_t sizeId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(1));

    IntervalValue size = as[sizeId].getInterval();
    if (size.isBottom()) {
      size = IntervalValue(0, 0);
    }

    bool isSafe = canSafelyAccessMemory(as, ptrId, size);

    if (isSafe) {
      llvm::outs()
          << "success: expected safe buffer access at SAFE_BUFACCESS - "
          << *call << "\n";
    } else {
      llvm::errs() << "failure: unexpected buffer overflow at SAFE_BUFACCESS\n";
      assert(false && "SAFE_BUFACCESS checkpoint failed");
    }
  } else if (funcName == "UNSAFE_BUFACCESS") {
    AbstractInterpretation::getAEInstance().markCheckpointChecked(call);
    AbstractInterpretation::getAEInstance().checkpoints.erase(call);
    if (call->arg_size() < 2)
      return;

    AbstractState &as =
        AbstractInterpretation::getAEInstance().getAbsStateFromTrace(call);

    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));
    uint32_t sizeId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(1));

    IntervalValue size = as[sizeId].getInterval();
    if (size.isBottom()) {
      if (const auto *ci =
              llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1))) {
        size = IntervalValue(ci->getSExtValue(), ci->getSExtValue());
      } else {
        size = IntervalValue::top();
      }
    }

    bool isSafe = canSafelyAccessMemory(as, ptrId, size);

    if (!isSafe) {
      llvm::outs() << "success: expected buffer overflow at UNSAFE_BUFACCESS - "
                   << *call << "\n";
    } else {
      llvm::errs() << "failure: buffer overflow expected at UNSAFE_BUFACCESS, "
                      "but none detected\n";
      assert(false && "UNSAFE_BUFACCESS checkpoint failed");
    }
  }
}

void BufOverflowDetector::reportBug() {
  if (!instToBugInfo.empty()) {
    llvm::errs() << "###################### Buffer Overflow ("
                 << instToBugInfo.size() << " found) ######################\n";
    for (const auto &it : instToBugInfo) {
      llvm::errs() << it.second << "\n";
    }
  }
}

void BufOverflowDetector::reset() {
  clearEventTrace();
  bugLoc.clear();
  instToBugInfo.clear();
  gepObjOffsetFromBase.clear();
  gepObjOffsetFromBaseByObjId.clear();
}

void BufOverflowDetector::addBugToReporter(const AEException &e,
                                           const llvm::Instruction *inst) {
  std::string loc;
  if (const llvm::DILocation *debugLoc = inst->getDebugLoc()) {
    loc = debugLoc->getFilename().str() + ":" +
          std::to_string(debugLoc->getLine());
  } else {
    loc = "unknown location";
  }

  if (bugLoc.find(loc) != bugLoc.end())
    return;

  bugLoc.insert(loc);
  instToBugInfo[inst] = std::string(e.what()) + " @ " + loc;
  emitAEBugReport(kind, inst, std::string(e.what()));
}

bool BufOverflowDetector::canSafelyAccessMemory(AbstractState &as,
                                                uint32_t ptrId,
                                                const IntervalValue &len) {
  if (!as.inVarToAddrsTable(ptrId))
    return true;

  const AbstractValue &absVal = as[ptrId];
  if (!absVal.isAddr())
    return true;

  for (const auto &addr : absVal.getAddrs()) {
    if (AbstractState::isInvalidMem(addr))
      return false;
    if (AbstractState::isNullMem(addr))
      return false;

    uint32_t objId = as.getIDFromAddr(addr);
    uint32_t objSize = as.getObjSize(objId);

    if (objSize > 0 &&
        len.ub().getIntNumeral() > static_cast<int64_t>(objSize)) {
      return false;
    }
  }

  return true;
}

void BufOverflowDetector::initExtAPIBufOverflowCheckRules() {
  // Memory copy functions - check both destination and source
  extAPIBufOverflowCheckRules["llvm.memcpy.p0i8.p0i8.i64"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memcpy.p0.p0.i64"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memcpy.p0i8.p0i8.i32"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memcpy.p0i8.p0i8.i16"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memcpy.p0i8.p0i8.i8"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memcpy"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memmove"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memmove.p0i8.p0i8.i64"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memmove.p0.p0.i64"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memmove.p0i8.p0i8.i32"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["__memcpy_chk"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["memmove"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["bcopy"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["memccpy"] = {{0, 3}, {1, 3}};
  extAPIBufOverflowCheckRules["__memmove_chk"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["__bcopy"] = {{0, 2}, {1, 2}};

  // Memory set functions - check destination
  extAPIBufOverflowCheckRules["llvm.memset.p0i8.i32"] = {{0, 2}};
  extAPIBufOverflowCheckRules["llvm.memset.p0i8.i64"] = {{0, 2}};
  extAPIBufOverflowCheckRules["llvm.memset.p0.i64"] = {{0, 2}};
  extAPIBufOverflowCheckRules["llvm.memset.p0i8.i8"] = {{0, 2}};
  extAPIBufOverflowCheckRules["llvm.memset"] = {{0, 2}};
  extAPIBufOverflowCheckRules["__memset_chk"] = {{0, 2}};
  extAPIBufOverflowCheckRules["wmemset"] = {{0, 2}};
  extAPIBufOverflowCheckRules["bzero"] = {{0, 1}};

  // String copy functions - check destination and source
  extAPIBufOverflowCheckRules["strcpy"] = {{0, 1}};
  extAPIBufOverflowCheckRules["strncpy"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["stpcpy"] = {{0, 1}};
  extAPIBufOverflowCheckRules["strcat"] = {{0, 1}};
  extAPIBufOverflowCheckRules["strncat"] = {{0, 2}};
  extAPIBufOverflowCheckRules["__strcpy_chk"] = {{0, 1}};
  extAPIBufOverflowCheckRules["__strncpy_chk"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["__strcat_chk"] = {{0, 1}};
  extAPIBufOverflowCheckRules["__strncat_chk"] = {{0, 2}};

  // Wide string functions
  extAPIBufOverflowCheckRules["wcscpy"] = {{0, 1}};
  extAPIBufOverflowCheckRules["wcsncpy"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["wcscat"] = {{0, 1}};
  extAPIBufOverflowCheckRules["wcsncat"] = {{0, 2}};
  extAPIBufOverflowCheckRules["__wcscpy_chk"] = {{0, 1}};
  extAPIBufOverflowCheckRules["__wcsncpy_chk"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["__wcscat_chk"] = {{0, 1}};
  extAPIBufOverflowCheckRules["__wcsncat_chk"] = {{0, 2}};

  // I/O functions
  extAPIBufOverflowCheckRules["fgets"] = {{0, 2}};
  extAPIBufOverflowCheckRules["fread"] = {{0, 2}};
  extAPIBufOverflowCheckRules["fwrite"] = {{0, 2}};

  // iconv
  extAPIBufOverflowCheckRules["iconv"] = {{1, 2}, {3, 4}};
}

void BufOverflowDetector::detectExtAPI(AbstractState &as,
                                       const llvm::CallBase *call) {
  if (!call->getCalledFunction())
    return;

  const llvm::Function *callee = call->getCalledFunction();
  std::string funcName = callee->getName().str();

  // First, check for BUF_CHECK annotations from AEExtAPI
  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  AEExtAPI *utils = ae.getUtils();
  if (utils) {
    std::vector<std::string> annotations = utils->getExtFuncAnnotations(callee);
    for (const auto &annotation : annotations) {
      if (annotation.find("BUF_CHECK:") == 0) {
        // Parse BUF_CHECK:ArgN,ArgM format
        std::string args = annotation.substr(10);
        size_t commaPos = args.find(',');
        if (commaPos != std::string::npos) {
          try {
            uint32_t bufArg = std::stoi(args.substr(0, commaPos));
            uint32_t sizeArg = std::stoi(args.substr(commaPos + 1));

            if (call->arg_size() > bufArg && call->arg_size() > sizeArg) {
              uint32_t bufId = AbstractInterpretation::getValueIdStatic(
                  call->getArgOperand(bufArg));
              uint32_t lenId = AbstractInterpretation::getValueIdStatic(
                  call->getArgOperand(sizeArg));

              if (!as.inVarToValTable(lenId))
                continue;

              IntervalValue len = as[lenId].getInterval();
              if (!canSafelyAccessMemory(as, bufId, len)) {
                AEException bug("Buffer overflow in " + funcName +
                                ": access length " + len.toString() +
                                " may exceed buffer bounds");
                addBugToReporter(bug, call);
              }
            }
          } catch (const std::invalid_argument &) {
            // Skip malformed BUF_CHECK annotation
            continue;
          } catch (const std::out_of_range &) {
            // Skip BUF_CHECK annotation with out-of-range values
            continue;
          }
        }
      }
    }
  }

  // Fallback to rules map
  auto it = extAPIBufOverflowCheckRules.find(funcName);
  if (it == extAPIBufOverflowCheckRules.end())
    return;

  for (const auto &arg : it->second) {
    if (call->arg_size() <= arg.first || call->arg_size() <= arg.second)
      continue;

    uint32_t bufId = AbstractInterpretation::getValueIdStatic(
        call->getArgOperand(arg.first));
    uint32_t lenId = AbstractInterpretation::getValueIdStatic(
        call->getArgOperand(arg.second));

    if (!as.inVarToValTable(lenId))
      continue;

    IntervalValue len = as[lenId].getInterval();
    if (!canSafelyAccessMemory(as, bufId, len)) {
      AEException bug("Buffer overflow in " + funcName + ": access length " +
                      len.toString() + " may exceed buffer bounds");
      addBugToReporter(bug, call);
    }
  }

  // Check for string functions
  if (funcName.find("strcpy") != std::string::npos) {
    detectStrcpy(as, call);
  } else if (funcName.find("strcat") != std::string::npos) {
    detectStrcat(as, call);
  }
}

bool BufOverflowDetector::detectStrcpy(AbstractState &as,
                                       const llvm::CallBase *call) {
  if (call->arg_size() < 2)
    return true;

  uint32_t dstId =
      AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));
  uint32_t srcId =
      AbstractInterpretation::getValueIdStatic(call->getArgOperand(1));

  if (!as.inVarToAddrsTable(dstId) || !as.inVarToAddrsTable(srcId))
    return true;

  // Get source string length using getStrlen utility
  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  AEExtAPI *utils = ae.getUtils();
  if (!utils) {
    // Fallback to conservative estimate if utils not available
    IntervalValue srcLen(0, 1024);
    uint32_t dstSize = 0;
    for (auto addr : as[dstId].getAddrs()) {
      uint32_t objId = as.getIDFromAddr(addr);
      uint32_t size = as.getObjSize(objId);
      if (size > dstSize)
        dstSize = size;
    }
    if (dstSize > 0 &&
        srcLen.ub().getIntNumeral() > static_cast<int64_t>(dstSize)) {
      AEException bug("Buffer overflow in strcpy: source string length may "
                      "exceed destination buffer size");
      addBugToReporter(bug, call);
      return false;
    }
    return true;
  }

  IntervalValue srcLen = utils->getStrlen(as, srcId);

  // Get destination buffer size
  uint32_t dstSize = 0;
  for (auto addr : as[dstId].getAddrs()) {
    uint32_t objId = as.getIDFromAddr(addr);
    uint32_t size = as.getObjSize(objId);
    if (size > dstSize)
      dstSize = size;
  }

  // Check if source string length exceeds destination buffer size
  // Account for null terminator: need dstSize >= srcLen + 1
  if (dstSize > 0) {
    int64_t requiredSize = srcLen.ub().getIntNumeral() + 1;
    if (requiredSize > static_cast<int64_t>(dstSize)) {
      AEException bug("Buffer overflow in strcpy: source string length [" +
                      srcLen.toString() +
                      "] + 1 exceeds destination buffer "
                      "size " +
                      std::to_string(dstSize));
      addBugToReporter(bug, call);
      return false;
    }
  }

  return true;
}

bool BufOverflowDetector::detectStrcat(AbstractState &as,
                                       const llvm::CallBase *call) {
  if (call->arg_size() < 2)
    return true;

  uint32_t dstId =
      AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));
  uint32_t srcId =
      AbstractInterpretation::getValueIdStatic(call->getArgOperand(1));

  if (!as.inVarToAddrsTable(dstId) || !as.inVarToAddrsTable(srcId))
    return true;

  // Get string lengths using getStrlen utility
  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  AEExtAPI *utils = ae.getUtils();
  if (!utils) {
    // Fallback to conservative estimate if utils not available
    IntervalValue dstLen(0, 512);
    IntervalValue srcLen(0, 512);
    uint32_t dstSize = 0;
    for (auto addr : as[dstId].getAddrs()) {
      uint32_t objId = as.getIDFromAddr(addr);
      uint32_t size = as.getObjSize(objId);
      if (size > dstSize)
        dstSize = size;
    }
    IntervalValue totalLen = dstLen + srcLen;
    if (dstSize > 0 &&
        totalLen.ub().getIntNumeral() > static_cast<int64_t>(dstSize)) {
      AEException bug(
          "Buffer overflow in strcat: concatenated string may exceed "
          "destination buffer size");
      addBugToReporter(bug, call);
      return false;
    }
    return true;
  }

  IntervalValue dstLen = utils->getStrlen(as, dstId);
  IntervalValue srcLen = utils->getStrlen(as, srcId);

  // Get destination buffer size
  uint32_t dstSize = 0;
  for (auto addr : as[dstId].getAddrs()) {
    uint32_t objId = as.getIDFromAddr(addr);
    uint32_t size = as.getObjSize(objId);
    if (size > dstSize)
      dstSize = size;
  }

  // Check if concatenated string length exceeds destination buffer size
  // Account for null terminator: need dstSize >= dstLen + srcLen + 1
  IntervalValue totalLen = dstLen + srcLen;
  if (dstSize > 0) {
    int64_t requiredSize = totalLen.ub().getIntNumeral() + 1;
    if (requiredSize > static_cast<int64_t>(dstSize)) {
      AEException bug(
          "Buffer overflow in strcat: concatenated string length [" +
          totalLen.toString() +
          "] + 1 exceeds destination buffer "
          "size " +
          std::to_string(dstSize));
      addBugToReporter(bug, call);
      return false;
    }
  }

  return true;
}

/// @brief Detects null pointer dereference issues.
///
/// Checks load instructions, store instructions, and GEP instructions for
/// potential null pointer dereferences by analyzing whether pointers can
/// point to null or invalid memory.
///
/// @param as Reference to the abstract state.
/// @param inst Pointer to the instruction to analyze.
void NullptrDerefDetector::detect(AbstractState &as,
                                  const llvm::Instruction *inst) {
  auto hasDefiniteNonNullBase = [](const llvm::Value *ptrVal) -> bool {
    const llvm::Value *base = llvm::getUnderlyingObject(ptrVal, 16);
    return llvm::isa<llvm::AllocaInst>(base) ||
           llvm::isa<llvm::GlobalValue>(base);
  };

  // Check for null pointer dereference in load instructions
  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
    if (hasDefiniteNonNullBase(load->getPointerOperand()))
      return;
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(load->getPointerOperand());

    // Check if pointer operand is a null constant
    const llvm::Value *ptrVal = load->getPointerOperand();
    bool isNullConstant = llvm::isa<llvm::ConstantPointerNull>(ptrVal);

    bool isSafe = canSafelyDerefPtr(as, ptrId);
    if (!isSafe) {
      std::string bugMsg = "Null pointer dereference at load instruction";
      if (isNullConstant) {
        bugMsg += " (null constant)";
      }
      AEException bug(bugMsg);
      addBugToReporter(bug, inst);
    }
  }

  // Check for null pointer dereference in store instructions
  if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
    if (hasDefiniteNonNullBase(store->getPointerOperand()))
      return;
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(store->getPointerOperand());

    // Check if pointer operand is a null constant
    const llvm::Value *ptrVal = store->getPointerOperand();
    bool isNullConstant = llvm::isa<llvm::ConstantPointerNull>(ptrVal);

    bool isSafe = canSafelyDerefPtr(as, ptrId);
    if (!isSafe) {
      std::string bugMsg = "Null pointer dereference at store instruction";
      if (isNullConstant) {
        bugMsg += " (null constant)";
      }
      AEException bug(bugMsg);
      addBugToReporter(bug, inst);
    }
  }

  // Check for null pointer dereference in GEP instructions
  if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(inst)) {
    if (hasDefiniteNonNullBase(gep->getPointerOperand()))
      return;
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(gep->getPointerOperand());
    if (!canSafelyDerefPtr(as, ptrId)) {
      AEException bug("Null pointer dereference at GEP instruction");
      addBugToReporter(bug, inst);
    }
  }

  if (const auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
    if (const llvm::Function *callee = call->getCalledFunction()) {
      AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
      AEExtAPI *utils = ae.getUtils();
      bool shouldCheck = false;

      if (utils) {
        AEExtAPI::ExtAPIType extType = utils->getExtAPIType(callee);
        shouldCheck =
            (extType == AEExtAPI::MEMCPY || extType == AEExtAPI::MEMSET ||
             extType == AEExtAPI::STRCPY || extType == AEExtAPI::STRCAT);
      } else {
        // Fallback to string matching
        std::string funcName = callee->getName().str();
        shouldCheck = (funcName.find("memcpy") != std::string::npos ||
                       funcName.find("memset") != std::string::npos ||
                       funcName.find("strcpy") != std::string::npos ||
                       funcName.find("strcat") != std::string::npos);
      }

      if (shouldCheck) {
        detectExtAPI(as, call);
      }
    }
  }
}

void NullptrDerefDetector::detectExtAPI(AbstractState &as,
                                        const llvm::CallBase *call) {
  const llvm::Function *callee = call->getCalledFunction();
  if (!callee)
    return;

  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  AEExtAPI *utils = ae.getUtils();

  std::vector<uint32_t> pointerArgs;
  if (utils) {
    for (const std::string &annotation : utils->getExtFuncAnnotations(callee)) {
      if (annotation.find("MEMCPY") != std::string::npos) {
        if (call->arg_size() < 4) {
          pointerArgs.push_back(0);
          pointerArgs.push_back(1);
        } else {
          pointerArgs.push_back(1);
          pointerArgs.push_back(2);
          pointerArgs.push_back(3);
          pointerArgs.push_back(4);
        }
      } else if (annotation.find("MEMSET") != std::string::npos) {
        pointerArgs.push_back(0);
      } else if (annotation.find("STRCPY") != std::string::npos) {
        pointerArgs.push_back(0);
        pointerArgs.push_back(1);
      } else if (annotation.find("STRCAT") != std::string::npos) {
        pointerArgs.push_back(0);
        pointerArgs.push_back(1);
      }
    }
  }

  if (pointerArgs.empty()) {
    return;
  }

  std::sort(pointerArgs.begin(), pointerArgs.end());
  pointerArgs.erase(std::unique(pointerArgs.begin(), pointerArgs.end()),
                    pointerArgs.end());

  for (uint32_t argIdx : pointerArgs) {
    if (call->arg_size() <= argIdx)
      continue;
    uint32_t argId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(argIdx));
    if (!canSafelyDerefPtr(as, argId)) {
      AEException bug("Null pointer dereference in " + callee->getName().str() +
                      " argument " + std::to_string(argIdx));
      addBugToReporter(bug, call);
    }
  }
}

void NullptrDerefDetector::handleStubFunctions(const llvm::CallBase *call) {
  if (!call->getCalledFunction())
    return;

  std::string funcName = call->getCalledFunction()->getName().str();

  if (funcName == "SAFE_LOAD") {
    AbstractInterpretation::getAEInstance().markCheckpointChecked(call);
    AbstractInterpretation::getAEInstance().checkpoints.erase(call);
    if (call->arg_size() < 1)
      return;

    AbstractState &as =
        AbstractInterpretation::getAEInstance().getAbsStateFromTrace(call);
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));

    bool isSafe = canSafelyDerefPtr(as, ptrId);
    if (isSafe) {
      llvm::outs() << "success: expected safe dereference at SAFE_LOAD - "
                   << *call << "\n";
    } else {
      llvm::errs() << "failure: unexpected null dereference at SAFE_LOAD\n";
      assert(false && "SAFE_LOAD checkpoint failed");
    }
  } else if (funcName == "UNSAFE_LOAD") {
    AbstractInterpretation::getAEInstance().markCheckpointChecked(call);
    AbstractInterpretation::getAEInstance().checkpoints.erase(call);
    if (call->arg_size() < 1)
      return;

    AbstractState &as =
        AbstractInterpretation::getAEInstance().getAbsStateFromTrace(call);
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));

    bool isSafe = canSafelyDerefPtr(as, ptrId);
    if (!isSafe) {
      llvm::outs() << "success: expected null dereference at UNSAFE_LOAD - "
                   << *call << "\n";
    } else {
      llvm::errs() << "failure: null dereference expected at UNSAFE_LOAD, but "
                      "none detected\n";
      assert(false && "UNSAFE_LOAD checkpoint failed");
    }
  }
}

void NullptrDerefDetector::reportBug() {
  if (!instToBugInfo.empty()) {
    llvm::errs() << "###################### Nullptr Dereference ("
                 << instToBugInfo.size() << " found) ######################\n";
    for (const auto &it : instToBugInfo) {
      llvm::errs() << it.second << "\n";
    }
  } else {
    llvm::errs() << "###################### Nullptr Dereference (0 found) "
                    "######################\n";
  }
}

void NullptrDerefDetector::reset() {
  clearEventTrace();
  bugLoc.clear();
  instToBugInfo.clear();
}

void NullptrDerefDetector::addBugToReporter(const AEException &e,
                                            const llvm::Instruction *inst) {
  std::string loc;
  if (const llvm::DILocation *debugLoc = inst->getDebugLoc()) {
    loc = debugLoc->getFilename().str() + ":" +
          std::to_string(debugLoc->getLine());
  } else {
    // Fallback location for stripped/no-debug IR: use a stable per-instruction
    // textual key so multiple unknown-location bugs do not collapse into one.
    std::string instStr;
    llvm::raw_string_ostream os(instStr);
    inst->print(os);
    os.flush();
    const llvm::Function *func = inst->getFunction();
    const llvm::BasicBlock *bb = inst->getParent();
    loc = (func ? func->getName().str() : "unknown_function") +
          "::" + (bb && bb->hasName() ? bb->getName().str() : "unknown_bb") +
          "::" + std::to_string(inst->getOpcode()) + "::" + instStr;
  }

  if (bugLoc.find(loc) != bugLoc.end())
    return;

  bugLoc.insert(loc);
  instToBugInfo[inst] = std::string(e.what()) + " @ " + loc;
  emitAEBugReport(kind, inst, std::string(e.what()));
}

bool NullptrDerefDetector::canSafelyDerefPtr(AbstractState &as,
                                             uint32_t ptrId) {
  // Special case: if ptrId is 0 (NullPtr), check if it's a null constant
  // Check if the value exists in the abstract state
  bool hasValue = (as._varToAbsVal.find(ptrId) != as._varToAbsVal.end());

  // If ptrId is 0 (null pointer constant) and not in state, it's null
  if (ptrId == 0 && !hasValue) {
    return false; // Null pointer cannot be safely dereferenced
  }

  AbstractValue absVal = as[ptrId];

  // Uninitialized value cannot be dereferenced
  if (isUninit(absVal))
    return false;

  // Interval value (non-addr) is safe
  if (!absVal.isAddr())
    return true;

  // Check each address
  for (const auto &addr : absVal.getAddrs()) {
    if (AbstractState::isInvalidMem(addr))
      return false;
    if (AbstractState::isNullMem(addr))
      return false;
    if (as.isFreedMem(addr))
      return false;
  }

  return true;
}

IntervalValue
BufOverflowDetector::getAccessOffset(AbstractState &as, uint32_t objId,
                                     const llvm::GetElementPtrInst *gep) {
  // Get the offset from the GEP instruction
  IntervalValue offset = as.getByteOffset(gep);

  // If we have tracked offset by object ID, prefer it.
  auto objIt = gepObjOffsetFromBaseByObjId.find(objId);
  if (objIt != gepObjOffsetFromBaseByObjId.end()) {
    return objIt->second;
  }

  // If we have tracked offset from base for this GEP, use it
  // (it already includes accumulated offsets from nested GEPs)
  if (hasGepObjOffsetFromBase(gep)) {
    return getGepObjOffsetFromBase(gep);
  }

  // Otherwise, check if the pointer operand is a GEP and accumulate
  if (const auto *prevGep =
          llvm::dyn_cast<llvm::GetElementPtrInst>(gep->getPointerOperand())) {
    if (hasGepObjOffsetFromBase(prevGep)) {
      IntervalValue prevOffset = getGepObjOffsetFromBase(prevGep);
      return prevOffset + offset;
    }
  }

  return offset;
}

void BufOverflowDetector::updateGepObjOffsetFromBase(AbstractState &as,
                                                     AddressValue gepAddrs,
                                                     AddressValue objAddrs,
                                                     IntervalValue offset) {
  // Preserve SVF-like object-based propagation:
  // - Base object: gep offset = current offset
  // - GEP object:  gep offset = base offset + current offset
  for (const auto &objAddr : objAddrs) {
    uint32_t baseObjId = as.getIDFromAddr(objAddr);
    IntervalValue baseOffset(0, 0);
    auto baseIt = gepObjOffsetFromBaseByObjId.find(baseObjId);
    if (baseIt != gepObjOffsetFromBaseByObjId.end()) {
      baseOffset = baseIt->second;
    }

    IntervalValue accumulated = baseOffset + offset;
    for (const auto &gepAddr : gepAddrs) {
      uint32_t gepObjId = as.getIDFromAddr(gepAddr);
      auto it = gepObjOffsetFromBaseByObjId.find(gepObjId);
      if (it == gepObjOffsetFromBaseByObjId.end()) {
        gepObjOffsetFromBaseByObjId.emplace(gepObjId, accumulated);
      } else {
        it->second.join_with(accumulated);
      }
    }
  }
}

void BufOverflowDetector::addToGepObjOffsetFromBase(
    const llvm::GetElementPtrInst *gep, const IntervalValue &offset) {
  gepObjOffsetFromBase[gep] = offset;
}

bool BufOverflowDetector::hasGepObjOffsetFromBase(
    const llvm::GetElementPtrInst *gep) const {
  return gepObjOffsetFromBase.find(gep) != gepObjOffsetFromBase.end();
}

IntervalValue BufOverflowDetector::getGepObjOffsetFromBase(
    const llvm::GetElementPtrInst *gep) const {
  auto it = gepObjOffsetFromBase.find(gep);
  if (it != gepObjOffsetFromBase.end()) {
    return it->second;
  }
  return IntervalValue(0, 0);
}

//===----------------------------------------------------------------------===//
// UseAfterFreeDetector Implementation
//===----------------------------------------------------------------------===//

void UseAfterFreeDetector::detect(AbstractState &as,
                                  const llvm::Instruction *inst) {
  // Check for loads from freed memory
  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(load->getPointerOperand());

    if (mayAccessFreedMem(as, ptrId)) {
      addEventToTrace(AEBugEventType::LOAD, inst, "Load from freed memory");
      AEException bug("Use-after-free: load from freed memory");
      addBugToReporter(bug, inst);
    }
  }

  // Check for stores to freed memory
  if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(store->getPointerOperand());

    if (mayAccessFreedMem(as, ptrId)) {
      addEventToTrace(AEBugEventType::STORE, inst, "Store to freed memory");
      AEException bug("Use-after-free: store to freed memory");
      addBugToReporter(bug, inst);
    }
  }

  // Check for GEP on freed pointers
  if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(inst)) {
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(gep->getPointerOperand());

    if (mayAccessFreedMem(as, ptrId)) {
      addEventToTrace(AEBugEventType::DEREF, inst, "GEP on freed memory");
      AEException bug("Use-after-free: GEP on freed memory");
      addBugToReporter(bug, inst);
    }
  }
}

void UseAfterFreeDetector::handleStubFunctions(const llvm::CallBase *call) {
  // Track allocation/free events
}

void UseAfterFreeDetector::reportBug() {
  if (!instToBugInfo.empty()) {
    llvm::errs() << "###################### Use-After-Free ("
                 << instToBugInfo.size() << " found) ######################\n";
    for (const auto &it : instToBugInfo) {
      llvm::errs() << it.second << "\n";
    }
  }
}

void UseAfterFreeDetector::reset() {
  clearEventTrace();
  bugLoc.clear();
  instToBugInfo.clear();
}

bool UseAfterFreeDetector::mayAccessFreedMem(AbstractState &as,
                                             uint32_t ptrId) {
  if (!as.inVarToAddrsTable(ptrId))
    return false;

  const AbstractValue &absVal = as[ptrId];
  if (!absVal.isAddr())
    return false;

  for (const auto &addr : absVal.getAddrs()) {
    if (as.isFreedMem(addr))
      return true;
  }

  return false;
}

void UseAfterFreeDetector::addBugToReporter(const AEException &e,
                                            const llvm::Instruction *inst) {
  std::string loc;
  if (const llvm::DILocation *debugLoc = inst->getDebugLoc()) {
    loc = debugLoc->getFilename().str() + ":" +
          std::to_string(debugLoc->getLine());
  } else {
    loc = "unknown location";
  }

  if (bugLoc.find(loc) != bugLoc.end())
    return;

  bugLoc.insert(loc);
  instToBugInfo[inst] = std::string(e.what()) + " @ " + loc;
  emitAEBugReport(kind, inst, std::string(e.what()));
}

//===----------------------------------------------------------------------===//
// InvalidFreeDetector Implementation
//===----------------------------------------------------------------------===//

void InvalidFreeDetector::detect(AbstractState &as,
                                 const llvm::Instruction *inst) {
  // Check for free() calls
  if (const auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
    if (const llvm::Function *callee = call->getCalledFunction()) {
      std::string funcName = callee->getName().str();
      if (funcName == "free" || funcName == "cfree" ||
          funcName == "malloc_free") {
        if (call->arg_size() >= 1) {
          uint32_t ptrId =
              AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));

          if (!isValidFree(as, ptrId)) {
            addEventToTrace(AEBugEventType::FREE, inst,
                            "Invalid free detected");
            AEException bug("Invalid free: freeing invalid memory");
            addBugToReporter(bug, inst);
          }
        }
      }
    }
  }
}

void InvalidFreeDetector::handleStubFunctions(const llvm::CallBase *call) {
  // Track allocation/free events
}

void InvalidFreeDetector::reportBug() {
  if (!instToBugInfo.empty()) {
    llvm::errs() << "###################### Invalid Free ("
                 << instToBugInfo.size() << " found) ######################\n";
    for (const auto &it : instToBugInfo) {
      llvm::errs() << it.second << "\n";
    }
  }
}

void InvalidFreeDetector::reset() {
  clearEventTrace();
  bugLoc.clear();
  instToBugInfo.clear();
}

bool InvalidFreeDetector::isValidFree(AbstractState &as, uint32_t ptrId) {
  // If we don't track this pointer, assume valid to avoid false positives
  // (e.g. malloc-returned ptr not in var-to-addrs table from external/stub)
  if (!as.inVarToAddrsTable(ptrId))
    return true;

  const AbstractValue &absVal = as[ptrId];
  if (!absVal.isAddr())
    return false;

  for (const auto &addr : absVal.getAddrs()) {
    // Cannot free null or invalid memory
    if (AbstractState::isNullMem(addr))
      return false;
    if (AbstractState::isInvalidMem(addr))
      return false;
    // Check for double-free (already freed)
    if (as.isFreedMem(addr))
      return false;
  }

  return true;
}

void InvalidFreeDetector::addBugToReporter(const AEException &e,
                                           const llvm::Instruction *inst) {
  std::string loc;
  if (const llvm::DILocation *debugLoc = inst->getDebugLoc()) {
    loc = debugLoc->getFilename().str() + ":" +
          std::to_string(debugLoc->getLine());
  } else {
    loc = "unknown location";
  }

  if (bugLoc.find(loc) != bugLoc.end())
    return;

  bugLoc.insert(loc);
  instToBugInfo[inst] = std::string(e.what()) + " @ " + loc;
  emitAEBugReport(kind, inst, std::string(e.what()));
}

} // namespace analysis
} // namespace lotus
