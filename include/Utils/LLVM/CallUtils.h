#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Statepoint.h"

#include <vector>

namespace lotus {
namespace llvm_utils {

/// Resolve a syntactically direct call target.
///
/// This follows pointer casts and global aliases, but does not perform
/// call-graph construction or indirect-call resolution.
inline const llvm::Function *getDirectCallee(const llvm::CallBase *Call) {
  if (Call == nullptr) {
    return nullptr;
  }

  if (const auto *Statepoint = llvm::dyn_cast<llvm::GCStatepointInst>(Call)) {
    return Statepoint->getActualCalledFunction();
  }

  const llvm::Value *CalledOperand = Call->getCalledOperand();
  if (CalledOperand == nullptr) {
    return nullptr;
  }

  CalledOperand = CalledOperand->stripPointerCastsAndAliases();
  return llvm::dyn_cast<llvm::Function>(CalledOperand);
}

inline llvm::Function *getDirectCallee(llvm::CallBase *Call) {
  return const_cast<llvm::Function *>(
      getDirectCallee(static_cast<const llvm::CallBase *>(Call)));
}

/// Return the normal continuation instructions of a call-like instruction.
///
/// For invoke, this deliberately excludes the unwind destination. For callbr,
/// every successor is a normal continuation.
inline std::vector<const llvm::Instruction *>
getNormalCallContinuations(const llvm::CallBase *Call) {
  std::vector<const llvm::Instruction *> Continuations;
  if (Call == nullptr) {
    return Continuations;
  }

  if (const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Call)) {
    if (!Invoke->getNormalDest()->empty()) {
      Continuations.push_back(&Invoke->getNormalDest()->front());
    }
    return Continuations;
  }

  if (const auto *CallBr = llvm::dyn_cast<llvm::CallBrInst>(Call)) {
    for (unsigned I = 0, E = CallBr->getNumSuccessors(); I < E; ++I) {
      const auto *Successor = CallBr->getSuccessor(I);
      if (!Successor->empty()) {
        Continuations.push_back(&Successor->front());
      }
    }
    return Continuations;
  }

  if (const auto *Next = Call->getNextNode()) {
    Continuations.push_back(Next);
  }
  return Continuations;
}

inline std::vector<llvm::Instruction *>
getNormalCallContinuations(llvm::CallBase *Call) {
  std::vector<llvm::Instruction *> Continuations;
  for (const auto *Instruction :
       getNormalCallContinuations(static_cast<const llvm::CallBase *>(Call))) {
    Continuations.push_back(const_cast<llvm::Instruction *>(Instruction));
  }
  return Continuations;
}

} // namespace llvm_utils
} // namespace lotus
