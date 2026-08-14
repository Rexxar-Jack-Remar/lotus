#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

namespace lotus {
namespace llvm_utils {

/// Return the first instruction in a defined function's entry block.
inline const llvm::Instruction *
getFunctionEntryInstruction(const llvm::Function *Function) {
  if (Function == nullptr || Function->isDeclaration() || Function->empty() ||
      Function->getEntryBlock().empty()) {
    return nullptr;
  }
  return &Function->getEntryBlock().front();
}

inline llvm::Instruction *
getFunctionEntryInstruction(llvm::Function *Function) {
  return const_cast<llvm::Instruction *>(getFunctionEntryInstruction(
      static_cast<const llvm::Function *>(Function)));
}

inline bool isFunctionEntryInstruction(const llvm::Instruction *Instruction) {
  if (Instruction == nullptr || Instruction->getFunction() == nullptr) {
    return false;
  }
  return getFunctionEntryInstruction(Instruction->getFunction()) == Instruction;
}

} // namespace llvm_utils
} // namespace lotus
