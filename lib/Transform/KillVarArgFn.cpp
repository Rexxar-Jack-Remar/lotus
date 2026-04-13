#include "Transform/KillVarArgFn.h"

using namespace llvm;

bool killVarArgFunctions(Module &M) {
  bool changed = false;
  for (Function &F : M) {
    if (F.isVarArg()) {
      F.deleteBody();
      F.setComdat(nullptr);
      changed = true;
    }
  }
  return changed;
}

PreservedAnalyses KillVarArgFnPass::run(Module &M, ModuleAnalysisManager &) {
  bool changed = killVarArgFunctions(M);
  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
