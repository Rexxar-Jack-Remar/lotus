#ifndef TRANSFORM_KILLVARARGFN_H
#define TRANSFORM_KILLVARARGFN_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

/// Delete the bodies of all variadic functions, leaving only declarations.
/// Returns true if any function bodies were removed.
bool killVarArgFunctions(llvm::Module &M);

struct KillVarArgFnPass : llvm::PassInfoMixin<KillVarArgFnPass> {
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &);
};

#endif // TRANSFORM_KILLVARARGFN_H
