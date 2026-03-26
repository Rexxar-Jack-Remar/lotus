/**
 * @file RemoveExceptionHandlerPass.cpp
 * @brief Remove exception handling from functions.
 *
 * This pass removes exception handling by redirecting all invoke instructions'
 * unwind destinations to an unreachable basic block. This simplifies the IR
 * for pointer analysis, which doesn't need to model exception control flow.
 *
 * @author peiming
 */
#include "Alias/AserPTA/PreProcessing/Passes/RemoveExceptionHandlerPass.h"

#include "Alias/AserPTA/Util/Log.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

using namespace aser;
using namespace llvm;

/**
 * @brief Create an unreachable basic block for exception handlers.
 *
 * Creates a basic block containing only an unreachable instruction.
 * Used as the unwind destination for invoke instructions.
 *
 * @param F The function to add the block to
 * @return Pointer to the created unreachable basic block
 */
static BasicBlock *createUnReachableBB(Function &F) {
  auto *BB = BasicBlock::Create(F.getContext(), "aser.unreachable", &F);
  IRBuilder<> builder(BB);
  builder.CreateUnreachable();

  return BB;
}

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
 * Redirects all invoke instructions' unwind destinations to an unreachable
 * block, then eliminates unreachable blocks.
 *
 * @param F The function to process
 * @return true if any changes were made, false otherwise
 */
bool RemoveExceptionHandlerPass::runOnFunction(Function &F) {
  bool changed = false;
  BasicBlock *unReachableBB = nullptr;

  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *invokeInst = dyn_cast<InvokeInst>(&I)) {
        if (unReachableBB == nullptr) {
          unReachableBB = createUnReachableBB(F);
        }

        changed = true;
        invokeInst->setUnwindDest(unReachableBB);
      }
    }
  }

  if (changed) {
    EliminateUnreachableBlocks(F);
  }

  return changed;
}

char RemoveExceptionHandlerPass::ID = 0;
static RegisterPass<RemoveExceptionHandlerPass>
    REH("", "Remove Exception Handling Code in IR", false, /*CFG only*/
        false /*is analysis*/);
