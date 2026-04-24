/**
 * @file RemoveASMInstPass.cpp
 * @brief Remove inline assembly instructions from functions.
 *
 * This pass removes inline assembly (asm) instructions from functions by
 * replacing their uses with undef values. This simplifies the IR for pointer
 * analysis, which cannot analyze assembly code.
 *
 * @author peiming
 */
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/RemoveASMInstPass.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/NoFolder.h>

using namespace aser;
using namespace llvm;

/**
 * @brief Remove all inline assembly instructions from a function.
 *
 * Finds all call instructions that call inline assembly and replaces their
 * uses with undef values, then erases the instructions.
 *
 * @param F The function to process
 * @param builder IRBuilder for creating instructions (unused)
 * @return true if any ASM instructions were removed, false otherwise
 */
static bool destroyASMInst(Function &F, IRBuilder<NoFolder> &builder) {
  std::vector<Instruction *> removeThese;
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *callInst = dyn_cast<CallBase>(&I)) {
        auto *V = callInst->getCalledOperand();
        if (V != nullptr && isa<InlineAsm>(V)) {
          removeThese.push_back(callInst);
        }
      }
    }
  }

  for (auto *I : removeThese) {
    I->replaceAllUsesWith(llvm::UndefValue::get(I->getType()));
    I->eraseFromParent();
  }
  return !removeThese.empty();
}

/**
 * @brief Run the RemoveASMInstPass on a function.
 *
 * @param F The function to process
 * @return true if any changes were made, false otherwise
 */
bool RemoveASMInstPass::runOnFunction(llvm::Function &F) {
  IRBuilder<NoFolder> builder(F.getContext());

  bool changed = destroyASMInst(F, builder);
  return changed;
}

char RemoveASMInstPass::ID = 0;
static RegisterPass<RemoveASMInstPass> CIP("", "Remove ASM Instruction",
                                           true, /*CFG only*/
                                           false /*is analysis*/);
