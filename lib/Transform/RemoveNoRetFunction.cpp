
// RemoveNoRetFunction pass removes function bodies that are marked as never
// returning. This simplifies analysis by eliminating functions that cannot
// return normally.

#include "Transform/RemoveNoRetFunction.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Debug.h>

#define DEBUG_TYPE "remove-noret-function"

// New Pass Manager entry point. Removes function bodies that are marked as
// never returning.
PreservedAnalyses RemoveNoRetFunctionPass::run(Module &M,
                                               ModuleAnalysisManager &) {
  bool Changed = false;
  for (auto &F : M) {
    if (F.doesNotReturn() && !F.isDeclaration()) {
      F.deleteBody();
      F.setComdat(nullptr);
      Changed = true;
    }
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
