/**
 * @file RemoveExceptionHandlerPass.cpp
 * @brief Remove exception handling from functions.
 *
 * This pass removes exception handling by turning every invoke instruction into
 * a plain call. This simplifies the IR for pointer analysis, which doesn't need
 * to model exception control flow.
 *
 * @author peiming
 */
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/RemoveExceptionHandlerPass.h"

#include "Alias/InclusionBased/AserPTA/Util/Log.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Local.h>

using namespace aser;
using namespace llvm;

/**
 * @brief Initialize the RemoveExceptionHandlerPass.
 *
 * @param M The module (unused)
 * @return false (no module-level changes)
 */
bool RemoveExceptionHandlerPass::doInitialization(Module &M) {
  LOG_DEBUG("Processing Exception Handlers");
  return false;
}

/**
 * @brief Run the RemoveExceptionHandlerPass on a function.
 *
 * Converts all invoke instructions to calls, then eliminates unreachable blocks.
 *
 * @param F The function to process
 * @return true if any changes were made, false otherwise
 */
bool RemoveExceptionHandlerPass::runOnFunction(Function &F) {
  SmallVector<InvokeInst *, 8> Invokes;
  for (auto &BB : F)
    for (auto &I : BB)
      if (auto *invokeInst = dyn_cast<InvokeInst>(&I))
        Invokes.push_back(invokeInst);

  if (Invokes.empty())
    return false;

  // An unwind destination must be a landingpad; drop the unwind edge instead.
  for (auto *invokeInst : Invokes)
    changeToCall(invokeInst);

  EliminateUnreachableBlocks(F);

  return true;
}

char RemoveExceptionHandlerPass::ID = 0;
static RegisterPass<RemoveExceptionHandlerPass>
    REH("", "Remove Exception Handling Code in IR", false, /*CFG only*/
        false /*is analysis*/);
