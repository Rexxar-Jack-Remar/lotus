//===- AEDetector.h -- Vulnerability Detectors-----------------------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Checker/AE/AbstractState.h"

#include <cstddef>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

namespace lotus {
namespace analysis {

class AbstractInterpretation;
class AEExtAPI;

/// Base class for all detectors
class AEDetector {
public:
  enum DetectorKind { BUF_OVERFLOW, NULL_DEREF, UNKNOWN };

  AEDetector() : kind(UNKNOWN) {}
  virtual ~AEDetector() = default;

  static bool classof(const AEDetector *detector) {
    return detector->getKind() == AEDetector::UNKNOWN;
  }

  virtual void detect(AbstractState &as, const llvm::Instruction *inst) = 0;
  virtual void handleStubFunctions(const llvm::CallBase *call) = 0;
  virtual void reportBug() = 0;
  virtual size_t getBugCount() const = 0;

  DetectorKind getKind() const { return kind; }

protected:
  DetectorKind kind;
};

/// Exception class for handling errors in Abstract Execution
class AEException : public std::exception {
public:
  AEException(const std::string &message) : msg_(message) {}

  virtual const char *what() const throw() { return msg_.c_str(); }

private:
  std::string msg_;
};

/// Detector for identifying buffer overflow issues
class BufOverflowDetector : public AEDetector {
  friend class AbstractInterpretation;

public:
  BufOverflowDetector() {
    kind = BUF_OVERFLOW;
    initExtAPIBufOverflowCheckRules();
  }
  ~BufOverflowDetector() = default;

  static bool classof(const AEDetector *detector) {
    return detector->getKind() == AEDetector::BUF_OVERFLOW;
  }

  void detect(AbstractState &as, const llvm::Instruction *inst) override;
  void handleStubFunctions(const llvm::CallBase *call) override;
  void reportBug() override;
  size_t getBugCount() const override { return instToBugInfo.size(); }

  void detectExtAPI(AbstractState &as, const llvm::CallBase *call);
  bool canSafelyAccessMemory(AbstractState &as, uint32_t ptrId,
                             const IntervalValue &len);
  IntervalValue getAccessOffset(AbstractState &as, uint32_t objId,
                                const llvm::GetElementPtrInst *gep);
  void updateGepObjOffsetFromBase(AbstractState &as, AddressValue gepAddrs,
                                  AddressValue objAddrs, IntervalValue offset);
  void addToGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep,
                                 const IntervalValue &offset);
  bool hasGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep) const;
  IntervalValue
  getGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep) const;

private:
  void initExtAPIBufOverflowCheckRules();
  bool detectStrcpy(AbstractState &as, const llvm::CallBase *call);
  bool detectStrcat(AbstractState &as, const llvm::CallBase *call);
  void addBugToReporter(const AEException &e, const llvm::Instruction *inst);

  std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>>
      extAPIBufOverflowCheckRules;
  std::set<std::string> bugLoc;
  std::map<const llvm::Instruction *, std::string> instToBugInfo;
  std::map<const llvm::GetElementPtrInst *, IntervalValue> gepObjOffsetFromBase;
};

/// Detector for identifying null pointer dereference issues
class NullptrDerefDetector : public AEDetector {
  friend class AbstractInterpretation;

public:
  NullptrDerefDetector() { kind = NULL_DEREF; }
  ~NullptrDerefDetector() = default;

  static bool classof(const AEDetector *detector) {
    return detector->getKind() == AEDetector::NULL_DEREF;
  }

  void detect(AbstractState &as, const llvm::Instruction *inst) override;
  void handleStubFunctions(const llvm::CallBase *call) override;
  void reportBug() override;
  size_t getBugCount() const override { return instToBugInfo.size(); }

  void detectExtAPI(AbstractState &as, const llvm::CallBase *call);
  bool canSafelyDerefPtr(AbstractState &as, uint32_t ptrId);

  bool isUninit(const AbstractValue &v) {
    return v.getAddrs().isBottom() && v.getInterval().isBottom();
  }

  bool isNull(const AbstractValue &v) { return !v.isAddr() && !v.isInterval(); }

private:
  void addBugToReporter(const AEException &e, const llvm::Instruction *inst);

  std::set<std::string> bugLoc;
  std::map<const llvm::Instruction *, std::string> instToBugInfo;
};

} // namespace analysis
} // namespace lotus
