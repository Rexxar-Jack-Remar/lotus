/// @file ICFGUtils.h
/// @brief Shared LLVM control-flow predicates used by the ICFG.

#pragma once

#include <llvm/IR/Instructions.h>

namespace lotus::icfg {

/// Returns true when an EH terminator can unwind out of its function.
inline bool isExceptionalFunctionExitInst(const llvm::Instruction &inst) {
  if (llvm::isa<llvm::ResumeInst>(inst))
    return true;

  if (const auto *cleanupRet = llvm::dyn_cast<llvm::CleanupReturnInst>(&inst))
    return cleanupRet->unwindsToCaller();

  if (const auto *catchSwitch = llvm::dyn_cast<llvm::CatchSwitchInst>(&inst))
    return catchSwitch->unwindsToCaller();

  return false;
}

} // namespace lotus::icfg
