#include "Verification/Transform/BreakCritLoops.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace {

bool CloneMetadata(const Instruction *src, Instruction *dst) {
  if (src->hasMetadata()) {
    dst->copyMetadata(*src);
    return true;
  }
  return false;
}

} // namespace

namespace lotus {
namespace verification {
namespace transform {

llvm::PreservedAnalyses BreakCritLoopsPass::run(Function &F,
                                                FunctionAnalysisManager &) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  std::vector<BasicBlock *> toProcess;

  for (BasicBlock &BB : F) {
    if (BB.size() <= 1)
      continue;

    auto *Term = BB.getTerminator();
    if (auto *BI = dyn_cast<BranchInst>(Term)) {
      if (BI->isUnconditional())
        continue;

      for (BasicBlock *SuccBB : BI->successors()) {
        BasicBlock *UniqueSucc = SuccBB->getUniqueSuccessor();
        if (UniqueSucc && UniqueSucc == &BB) {
          toProcess.push_back(&BB);
          break;
        }
      }
    }
  }

  bool Changed = false;
  for (BasicBlock *BB : toProcess) {
    BasicBlock::iterator SplitPoint = --BB->end();
    BB->splitBasicBlock(SplitPoint, "crit.blk.split");
    if (!CloneMetadata(BB->getTerminator(), BB->getTerminator())) {
      errs() << "[BreakCritLoops] Failed assigning metadata\n";
    }
    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace transform
} // namespace verification
} // namespace lotus

namespace lotus {
namespace verification {
namespace transform {

BreakCritLoopsPass createBreakCritLoopsPass() { return BreakCritLoopsPass(); }

} // namespace transform
} // namespace verification
} // namespace lotus
