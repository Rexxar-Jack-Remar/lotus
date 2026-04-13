#ifndef TRANSFORM_DUMMYEXITBLOCK_H
#define TRANSFORM_DUMMYEXITBLOCK_H

#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"

/// Insert a dummy return block into functions that have no return instruction.
/// Returns true if a block was added.
bool addDummyExitBlock(llvm::Function &F);

struct DummyExitBlockPass : llvm::PassInfoMixin<DummyExitBlockPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &);
};

#endif // TRANSFORM_DUMMYEXITBLOCK_H
