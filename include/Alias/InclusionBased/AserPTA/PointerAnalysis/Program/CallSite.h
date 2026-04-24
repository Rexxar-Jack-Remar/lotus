//
// Created by peiming on 11/5/19.
// Updated for modern LLVM compatibility
//
#ifndef ASER_PTA_CALLSITE_H
#define ASER_PTA_CALLSITE_H

#include "Alias/InclusionBased/AserPTA/Util/Util.h"

#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>

namespace aser {

// wrapper around llvm::CallBase (replaces deprecated llvm::CallSite),
// but resolve constant expression evaluated to a function
class CallSite {
private:
  const llvm::CallBase *CB;
  static const llvm::Function *resolveTargetFunction(const llvm::Value *);

public:
  explicit CallSite(const llvm::Instruction *I)
      : CB(llvm::dyn_cast<llvm::CallBase>(I)) {}

  __attribute__((warn_unused_result)) inline bool isCallOrInvoke() const {
    return CB &&
           (llvm::isa<llvm::CallInst>(CB) || llvm::isa<llvm::InvokeInst>(CB));
  }

  __attribute__((warn_unused_result)) inline bool isIndirectCall() const {
    if (!CB)
      return false;
    if (CB->isIndirectCall()) {
      return true;
    }

    if (CB->getCalledFunction() != nullptr) {
      return false;
    }

    if (resolveTargetFunction(CB->getCalledOperand()) != nullptr) {
      return false;
    }

    auto *V = CB->getCalledOperand();
    if (auto *C = llvm::dyn_cast<llvm::Constant>(V)) {
      if (C->isNullValue()) {
        return true;
      }
    }

    return true;
  }

  __attribute__((warn_unused_result)) inline const llvm::Value *getCalledValue() const {
    return CB ? CB->getCalledOperand() : nullptr;
  }

  __attribute__((warn_unused_result)) inline const llvm::Function *getCalledFunction() const {
    return this->getTargetFunction();
  }

  __attribute__((warn_unused_result)) inline const llvm::Function *getTargetFunction() const {
    if (!CB || this->isIndirectCall()) {
      return nullptr;
    }
    auto *targetFunction = CB->getCalledFunction();
    if (targetFunction != nullptr) {
      return targetFunction;
    }

    return resolveTargetFunction(CB->getCalledOperand());
  }

  __attribute__((warn_unused_result))
  inline const llvm::Value *getReturnedArgOperand() const {
    return CB ? CB->getReturnedArgOperand() : nullptr;
  }

  __attribute__((warn_unused_result))
  inline const llvm::Instruction *getInstruction() const {
    return CB;
  }

  __attribute__((warn_unused_result))
  unsigned int getNumArgOperands() const {
    return CB ? CB->arg_size() : 0;
  }

  const llvm::Value *getArgOperand(unsigned int i) const {
    return CB ? CB->getArgOperand(i) : nullptr;
  }

  inline auto args() const { return CB->args(); }

  __attribute__((warn_unused_result))
  inline auto arg_begin() const {
    return CB->arg_begin();
  }

  __attribute__((warn_unused_result))
  inline auto arg_end() const {
    return CB->arg_end();
  }

  inline llvm::Type *getType() const { return CB ? CB->getType() : nullptr; }
};

} // namespace aser

#endif
