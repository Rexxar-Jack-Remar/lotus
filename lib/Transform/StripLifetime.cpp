#include "Transform/StripLifetime.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

bool stripLifetimeIntrinsics(Module &M) {
  bool changed = false;
  for (Function &F : M) {
    if (!F.isIntrinsic())
      continue;
    Intrinsic::ID id = F.getIntrinsicID();
    if (id != Intrinsic::lifetime_start && id != Intrinsic::lifetime_end)
      continue;
    while (!F.use_empty()) {
      auto *CI = cast<CallInst>(F.user_back());
      Value *last = *(CI->arg_end() - 1);
      CI->eraseFromParent();
      RecursivelyDeleteTriviallyDeadInstructions(last);
      changed = true;
    }
  }
  return changed;
}

PreservedAnalyses StripLifetimePass::run(Module &M, ModuleAnalysisManager &) {
  bool changed = stripLifetimeIntrinsics(M);
  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
