#pragma once

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

/// Remove all llvm.lifetime.start/end intrinsic calls from the module.
/// Returns true if any calls were removed.
bool stripLifetimeIntrinsics(llvm::Module &M);

struct StripLifetimePass : llvm::PassInfoMixin<StripLifetimePass> {
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &);
};

