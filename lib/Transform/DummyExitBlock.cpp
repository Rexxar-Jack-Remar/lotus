#include "Transform/DummyExitBlock.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

bool addDummyExitBlock(Function &F) {
  for (const BasicBlock &BB : F)
    if (isa<ReturnInst>(BB.getTerminator()))
      return false;

  BasicBlock *NewRet =
      BasicBlock::Create(F.getContext(), "DummyExitBlock", &F);
  if (F.getReturnType()->isVoidTy())
    ReturnInst::Create(F.getContext(), nullptr, NewRet);
  else
    ReturnInst::Create(F.getContext(),
                       Constant::getNullValue(F.getReturnType()), NewRet);
  return true;
}

PreservedAnalyses DummyExitBlockPass::run(Function &F,
                                          FunctionAnalysisManager &) {
  bool changed = addDummyExitBlock(F);
  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
