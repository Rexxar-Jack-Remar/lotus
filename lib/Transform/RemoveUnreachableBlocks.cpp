#include "Transform/RemoveUnreachableBlocks.h"

#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

PreservedAnalyses RemoveUnreachableBlocksPass::run(Function &F,
                                                   FunctionAnalysisManager &) {
  bool changed = removeUnreachableBlocks(F);
  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
